// Analyzer collection method, aggregation, MAP overlay, and mutation helpers.

// Analyzes a chain receiver, which permits terminal-only methods such as tap while typing the
// source because chain semantics depend on the receiver before the final method.
static char *analyze_chain_receiver(Node *e, Scope *s, AnalyzeCtx *ctx) {
    AnalyzeCtx receiver_ctx = *ctx;
    receiver_ctx.allow_terminal_tap = true;
    char *type = analyze_expr(e, s, &receiver_ctx);
    ctx->next_loop_id = receiver_ctx.next_loop_id;
    return type;
}
static void analyze_fn(FnInfo *fn, FnTable *fns, TypeTable *types, Scope *global_scope);

// Stores the checked type on a node, which gives later analyzer and emitter code a single source
// of truth because the AST is shared across phases.
static char *set_type(Node *n, const char *type) {
    n->checked_type = xstrdup(type);
    return n->checked_type;
}

// Analyzes a loop body with adjusted loop depth, which makes nested e/i shadowing explicit because
// qualified loop frames depend on depth.
static void analyze_loop_body(Node *body, Scope *loop_scope, AnalyzeCtx *ctx) {
    int saved_depth = ctx->loop_depth;
    ctx->loop_depth = saved_depth + 1;
    analyze_block(body, loop_scope, ctx);
    ctx->loop_depth = saved_depth;
}

// Wraps a REF read as an implicit dereference, which keeps read syntax lightweight because only
// caller-visible mutation needs the explicit mutation operator.
static char *wrap_expr_as_implicit_deref(Node *expr, const char *ref_type) {
    Node *inner = xmalloc(sizeof(Node));
    *inner = *expr;

    int line = expr->line;
    int col = expr->col;
    memset(expr, 0, sizeof(Node));
    expr->kind = NK_UNARY;
    expr->line = line;
    expr->col = col;
    expr->op = xstrdup("*");
    expr->expr = inner;
    expr->implicit_deref = true;
    return set_type(expr, ref_target_type(ref_type));
}

// Reads a REF when a value type is expected, which keeps argument and assignment checks ergonomic
// because REF values often flow into read-only positions.
static char *implicit_ref_read_if_assignable(Node *expr, const char *actual, const char *expected) {
    if (!expected || !is_ref_type(actual) || type_eq(actual, expected)) return (char *)actual;
    char *target = ref_target_type(actual);
    if (type_conforms_to_expected(target, expected) || type_assignable_to_expected(target, expected)) {
        return wrap_expr_as_implicit_deref(expr, actual);
    }
    return (char *)actual;
}

// Reads a REF for ordinary value use, which avoids leaking pointer mechanics into expressions
// because Caster treats REF as an alias unless mutation is requested.
static char *implicit_ref_read_value(Node *expr, const char *actual) {
    if (!is_ref_type(actual)) return (char *)actual;
    return wrap_expr_as_implicit_deref(expr, actual);
}

// Analyzes a name lookup, which attaches scope metadata and loop frame access because names can
// refer to locals, globals, functions, or contextual loop values.
static char *analyze_name(Node *n, Scope *s) {
    Symbol *sym = scope_try_resolve(s, n->name);
    if (sym) {
        if (sym->node && sym->node->kind == NK_FN) {
            die_at(n->line, n->col, "nested function '%s' can only be called directly inside its owning function", n->name);
        }
        if (sym->ownership == OWNERSHIP_FREED) {
            die_at(n->line, n->col, "use of freed value '%s'", n->name);
        }
        if (sym->c_expr) n->c_expr = xstrdup(sym->c_expr);
        return set_type(n, sym->type);
    }

    TypeInfo *info = type_find(g_types, n->name);
    if (info) {
        if (!type_has_complete_named_value(g_types, info)) {
            die_at(n->line, n->col, "MAP '%s' has no complete named value", n->name);
        }
        n->c_expr = named_value_storage_name(info->name);
        return set_type(n, info->kind == TYPEINFO_STRUCT ? info->name : info->target);
    }

    die_at(n->line, n->col, "unknown name '%s'", n->name);
    return NULL;
}

// Analyzes assignment targets, which validates writable shapes before values are checked because
// mutation and declaration inference share assignment syntax.
static char *analyze_lvalue(Node *target, Scope *s, AnalyzeCtx *ctx) {
    if (target->kind == NK_NAME) {
        Symbol *sym = scope_resolve(s, target->name, target);
        if (sym->c_expr) target->c_expr = xstrdup(sym->c_expr);
        return set_type(target, sym->type);
    }
    if (target->kind == NK_UNARY && target->op && strcmp(target->op, "*") == 0) {
        char *ref = analyze_lvalue(target->expr, s, ctx);
        if (!is_ref_type(ref)) die_at(target->line, target->col, "dereference assignment target needs REF, got %s", ref);
        return set_type(target, ref_target_type(ref));
    }
    if (target->kind == NK_DOT) {
        if (target->object && target->object->kind == NK_UNARY && target->object->op && strcmp(target->object->op, "*") == 0) {
            target->object->mutation_target = true;
        }
        char *object_type = analyze_expr(target->object, s, ctx);
        if (is_ref_type(object_type)) {
            die_at(target->line, target->col, "mutation through REF field access requires '*' on the mutation target");
        }
        return analyze_expr(target, s, ctx);
    }
    die_at(target->line, target->col, "cannot assign to %s", node_kind_name(target->kind));
    return NULL;
}

// Creates a synthetic INT node, which supports compiler-generated access expressions because some
// collection helpers need small AST nodes without source text.
static Node *make_int_node(int64_t value, Node *near) {
    Token tok = {TK_INT_LITERAL, xstrdup("1"), near->line, near->col};
    Node *n = node_new(NK_INT, tok);
    n->int_value = value;
    set_type(n, "INT");
    return n;
}

// Recognizes collection-transform methods, which keeps method body parsing and analysis aligned
// because upd/filt/agg/tap/group/sort share scoped e/i bindings.
static bool is_collection_method_name(const char *name) {
    return strcmp(name, "upd") == 0 || strcmp(name, "filt") == 0 || strcmp(name, "agg") == 0 ||
        strcmp(name, "tap") == 0 || strcmp(name, "group") == 0 || strcmp(name, "sort") == 0 ||
        strcmp(name, "any") == 0;
}

// Derives a singular loop alias from a plural source, which makes collection chains readable
// because users should not have to name obvious item variables.
static char *singular_alias_from_name(const char *name) {
    if (!name || !name[0]) return NULL;
    size_t len = strlen(name);
    if (len > 1 && name[len - 1] == 's') return xstrndup(name, len - 1);
    return xstrdup(name);
}

// Infers the item alias for a collection method, which gives chains names like order or item
// because explicit as aliases are no longer the common path.
static char *infer_collection_alias(Node *object) {
    if (!object) return NULL;
    if (object->kind == NK_NAME) return singular_alias_from_name(object->name);
    if (object->kind == NK_DOT && object->name) return singular_alias_from_name(object->name);
    if (object->kind == NK_METHOD_CALL) {
        if (object->result_alias) return xstrdup(object->result_alias);
        if (object->alias) return xstrdup(object->alias);
        return infer_collection_alias(object->object);
    }
    return NULL;
}

// Infers aliases for grouped results, which makes group output ergonomic because grouped map
// entries need stable e/val-style names.
static char *infer_group_result_alias(Node *key_expr) {
    if (!key_expr) return NULL;
    if (key_expr->kind == NK_DOT && key_expr->name) return xstrdup(key_expr->name);
    if (key_expr->kind == NK_NAME) return xstrdup(key_expr->name);
    return NULL;
}

// Defines an inferred method alias, which keeps e/i plus readable aliases in the same scope
// because collection bodies can use either style.
static void scope_define_alias(Scope *scope, Node *call, const char *alias, const char *type, const char *c_expr) {
    if (!alias || !alias[0]) return;
    if (strcmp(alias, "e") == 0 || strcmp(alias, "i") == 0 || strcmp(alias, "val") == 0) return;
    if (scope_lookup_current(scope, alias)) return;
    scope_define(scope, alias, type, c_expr, call);
}

// Creates a generated field node, which lets inferred structural results become named internal
// types because the emitter needs concrete field layouts.
static Node *synthetic_field(const char *type, const char *name) {
    Token tok = {TK_NAME, xstrdup((char *)name), 1, 1};
    Node *field = node_new(NK_FIELD, tok);
    field->declared_type = xstrdup(type);
    field->name = xstrdup(name);
    field->text = xstrdup(type);
    set_type(field, type);
    return field;
}

// Synthesizes a record type for structural literals, which preserves chainable anonymous data
// because C output still needs named structs.
static char *synthesize_record_type(TypeTable *types, Node *record, const char *prefix) {
    char *name = strf("__%s_%d_%d", prefix, record->line, record->col);
    TypeInfo *existing = type_find(types, name);
    if (existing) return xstrdup(existing->name);

    Token tok = {TK_MAP, xstrdup("MAP"), record->line, record->col};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup(name);
    for (int i = 0; i < record->fields.len; i++) {
        Node *entry = record->fields.items[i];
        if (!entry->name || !entry->value || !entry->value->checked_type) continue;
        vec_push(&decl->fields, synthetic_field(entry->value->checked_type, entry->name));
    }
    set_type(decl, name);

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
    return xstrdup(name);
}

// Synthesizes the MAP entry type used by group, which exposes key and val predictably because
// grouped data is a shaped intermediate value.
static char *synthesize_group_type(TypeTable *types, Node *call, const char *key_type, const char *element_type) {
    char *name = strf("__Group_%d_%d", call->line, call->col);
    TypeInfo *existing = type_find(types, name);
    if (existing) return xstrdup(existing->name);

    Token tok = {TK_MAP, xstrdup("MAP"), call->line, call->col};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup(name);
    vec_push(&decl->fields, synthetic_field(key_type, "key"));
    vec_push(&decl->fields, synthetic_field(array_type(element_type), "val"));
    set_type(decl, name);

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
    return xstrdup(name);
}

// Builds the scope for array/string methods, which injects e, i, and inferred aliases because
// method bodies are mini loop-like expressions.
static Scope *collection_method_scope(Node *call, Scope *parent, const char *element_type, const char *element_expr, AnalyzeCtx *ctx) {
    int id = ctx->next_loop_id++;
    call->index_name = strf("%s_i_%d", call->name, id);
    call->element_ptr = strf("%s_e_%d", call->name, id);
    const char *resolved_element_expr = element_expr ? element_expr : strf("(*%s)", call->element_ptr);
    if (!call->alias) call->alias = infer_collection_alias(call->object);
    if (!call->result_alias && call->alias) call->result_alias = xstrdup(call->alias);

    Scope *method_scope = scope_new(parent);
    scope_define(method_scope, "i", "INT", call->index_name, call);
    scope_define(method_scope, "e", element_type, resolved_element_expr, call);
    scope_define_alias(method_scope, call, call->alias, element_type, resolved_element_expr);
    return method_scope;
}

// Builds the scope for map methods, which injects e and val because map iteration treats the key
// as the primary element.
static Scope *map_method_scope(Node *call, Scope *parent, const char *key_type, const char *value_type, AnalyzeCtx *ctx) {
    int id = ctx->next_loop_id++;
    call->index_name = strf("%s_i_%d", call->name, id);
    call->element_ptr = strf("entry_%d", id);

    Scope *method_scope = scope_new(parent);
    scope_define(method_scope, "i", "INT", call->index_name, call);
    scope_define(method_scope, "e", key_type, strf("%s->key", call->element_ptr), call);
    scope_define(method_scope, "val", value_type, strf("%s->value", call->element_ptr), call);
    scope_define_alias(method_scope, call, call->alias, value_type, strf("%s->value", call->element_ptr));
    return method_scope;
}

// Rejects return statements inside method bodies, which keeps chains expression-oriented because
// collection methods should compose instead of becoming nested functions.
static void reject_method_body_ret(Node *n, const char *method_name) {
    if (!n) return;
    if (n->kind == NK_RET) {
        die_at(n->line, n->col, ".%s() method bodies produce their final expression; ret is not allowed", method_name);
    }

    reject_method_body_ret(n->body, method_name);
    reject_method_body_ret(n->then_block, method_name);
    reject_method_body_ret(n->else_block, method_name);
    reject_method_body_ret(n->value, method_name);
    reject_method_body_ret(n->target, method_name);
    reject_method_body_ret(n->expr, method_name);
    reject_method_body_ret(n->condition, method_name);
    reject_method_body_ret(n->object, method_name);
    reject_method_body_ret(n->index, method_name);
    reject_method_body_ret(n->callee, method_name);
    reject_method_body_ret(n->left, method_name);
    reject_method_body_ret(n->right, method_name);

    for (int i = 0; i < n->statements.len; i++) reject_method_body_ret(n->statements.items[i], method_name);
    for (int i = 0; i < n->args.len; i++) reject_method_body_ret(n->args.items[i], method_name);
    for (int i = 0; i < n->elements.len; i++) reject_method_body_ret(n->elements.items[i], method_name);
    for (int i = 0; i < n->fields.len; i++) reject_method_body_ret(n->fields.items[i], method_name);
    for (int i = 0; i < n->elx_branches.len; i++) reject_method_body_ret(n->elx_branches.items[i], method_name);
}

static char *analyze_method_value_block(Node *block, Scope *parent, AnalyzeCtx *ctx, const char *method_name);

// Analyzes one statement in a method body, which allows setup before the final value because
// upd/filt bodies can be readable without explicit ret.
static char *analyze_method_value_stmt(Node *st, Scope *scope, AnalyzeCtx *ctx, const char *method_name) {
    if (st->kind == NK_EXPR_STMT) {
        st->method_value = true;
        return analyze_expr(st->expr, scope, ctx);
    }

    if (st->kind == NK_IF) {
        char *condition_type = analyze_expr(st->condition, scope, ctx);
        if (!type_eq(condition_type, "BOL")) die_at(st->condition->line, st->condition->col, "if condition must be BOL, got %s", condition_type);

        char *branch_type = analyze_method_value_block(st->then_block, scope, ctx, method_name);
        for (int i = 0; i < st->elx_branches.len; i++) {
            Node *branch = st->elx_branches.items[i];
            char *elx_condition_type = analyze_expr(branch->condition, scope, ctx);
            if (!type_eq(elx_condition_type, "BOL")) die_at(branch->condition->line, branch->condition->col, "elx condition must be BOL, got %s", elx_condition_type);
            char *elx_type = analyze_method_value_block(branch->body, scope, ctx, method_name);
            if (!type_eq(elx_type, branch_type)) die_at(branch->line, branch->col, ".%s() if branches must produce the same type, got %s and %s", method_name, branch_type, elx_type);
        }

        if (!st->else_block) die_at(st->line, st->col, ".%s() value if needs an else branch", method_name);
        char *else_type = analyze_method_value_block(st->else_block, scope, ctx, method_name);
        if (!type_eq(else_type, branch_type)) die_at(st->else_block->line, st->else_block->col, ".%s() if branches must produce the same type, got %s and %s", method_name, branch_type, else_type);
        return branch_type;
    }

    die_at(st->line, st->col, ".%s() method body must end with an expression or if/else value", method_name);
    return NULL;
}

// Analyzes a method body as a value-producing block, which treats the last statement as the result
// because chained methods should stay compact.
static char *analyze_method_value_block(Node *block, Scope *parent, AnalyzeCtx *ctx, const char *method_name) {
    Scope *scope = scope_new(parent);
    if (!block || block->statements.len == 0) die_at(block ? block->line : 0, block ? block->col : 0, ".%s() method body needs a value", method_name);

    for (int i = 0; i < block->statements.len; i++) {
        Node *st = block->statements.items[i];
        if (st->kind == NK_FN) die_at(st->line, st->col, "nested functions are not allowed inside .%s() method bodies", method_name);
        if (i == block->statements.len - 1) return analyze_method_value_stmt(st, scope, ctx, method_name);
        analyze_stmt(st, scope, ctx);
    }

    die_at(block->line, block->col, ".%s() method body needs a value", method_name);
    return NULL;
}

// Analyzes a scoped collection method body, which applies local-style rules because method bodies
// behave like lightweight function bodies without returns.
static char *analyze_method_body(Node *call, Scope *method_scope, AnalyzeCtx *ctx) {
    reject_method_body_ret(call->body, call->name);

    AnalyzeCtx body_ctx = *ctx;
    body_ctx.enforce_local_style = true;

    char *value_type = analyze_method_value_block(call->body, method_scope, &body_ctx, call->name);
    ctx->next_loop_id = body_ctx.next_loop_id;
    return value_type;
}

// Detects accumulator-step aggregate bodies, which selects the modern agg form because aggregation
// now uses explicit += and - steps.
static bool agg_body_uses_steps(Node *body) {
    return body && body->statements.len > 0 && ((Node *)body->statements.items[0])->kind == NK_AGG_STEP;
}

// Combines aggregate step types, which allows INT/FLT promotion while rejecting mixed shapes
// because one accumulator must have one runtime type.
static char *unify_agg_step_type(const char *left, const char *right) {
    if (!left) return xstrdup(right);
    if (type_eq(left, right)) return xstrdup(left);
    char *numeric = numeric_binary_type(left, right);
    if (numeric) return xstrdup(numeric);
    return NULL;
}

// Analyzes accumulator steps, which verifies each contribution before emission because .agg lowers
// to a single typed reduction.
static char *analyze_agg_step_body(Node *call, Scope *method_scope, AnalyzeCtx *ctx) {
    reject_method_body_ret(call->body, call->name);

    AnalyzeCtx body_ctx = *ctx;
    body_ctx.enforce_local_style = true;
    char *value_type = NULL;

    for (int i = 0; i < call->body->statements.len; i++) {
        Node *step = call->body->statements.items[i];
        if (step->kind != NK_AGG_STEP) {
            die_at(step->line, step->col, ".agg() accumulator body only supports '+= expression' and '- expression' steps");
        }

        char *step_type = analyze_expr(step->value, method_scope, &body_ctx);
        if (!type_eq(step_type, "INT") && !type_eq(step_type, "FLT") && !type_eq(step_type, "STR")) {
            die_at(step->value->line, step->value->col, ".agg() step must produce INT, FLT, or STR, got %s", step_type);
        }
        if (step->op && strcmp(step->op, "-") == 0 && type_eq(step_type, "STR")) {
            die_at(step->line, step->col, ".agg() '-' steps require INT or FLT values");
        }
        char *next_type = unify_agg_step_type(value_type, step_type);
        if (!next_type) {
            die_at(step->value->line, step->value->col, ".agg() steps must produce one accumulator type, got %s and %s", value_type, step_type);
        }
        value_type = next_type;
    }

    ctx->next_loop_id = body_ctx.next_loop_id;
    return value_type ? value_type : xstrdup("INT");
}

// Analyzes aggregate bodies, which enforces the step-based form because old expression aggregates
// no longer define the language shape.
static char *analyze_agg_value(Node *call, Scope *method_scope, AnalyzeCtx *ctx, const char *element_type) {
    (void)element_type;
    if (call->body) {
        if (call->args.len != 0) die_at(call->line, call->col, ".agg() cannot mix expression arguments and method body");
        if (agg_body_uses_steps(call->body)) return analyze_agg_step_body(call, method_scope, ctx);
        die_at(call->line, call->col, ".agg() body only supports accumulator steps starting with += or -");
    }

    die_at(call->line, call->col, ".agg() expects accumulator steps starting with += or -");
    return xstrdup("INT");
}

// Recognizes starred collection method mutation, which separates in-place updates from
// value-producing chains because Caster makes caller-visible mutation syntactic.
static bool is_mutating_collection_method_expr(Node *expr) {
    if (!expr || expr->kind != NK_UNARY || !expr->op || strcmp(expr->op, "*") != 0) return false;
    Node *call = expr->expr;
    return call && call->kind == NK_METHOD_CALL &&
        (is_collection_method_name(call->name) || strcmp(call->name, "add") == 0);
}

// Recognizes starred collection joins, which lets plus mutate when explicitly requested because
// normal plus remains value-oriented.
static bool is_mutating_collection_plus_expr(Node *expr) {
    if (!expr || expr->kind != NK_BINARY || !expr->op || strcmp(expr->op, "+") != 0) return false;
    return expr->left && expr->left->kind == NK_UNARY && expr->left->op && strcmp(expr->left->op, "*") == 0;
}

// Rejects hidden global mutation, which forces mutation to appear in signatures or explicit refs
// because top-level values otherwise look copied.
static void reject_direct_global_mutation_target(Node *target, Scope *s, AnalyzeCtx *ctx) {
    if (!target || target->kind != NK_NAME) return;

    Symbol *receiver_sym = scope_try_resolve(s, target->name);
    if (receiver_sym && receiver_sym->is_global) {
        die_at(target->line, target->col, "direct mutation of top-level named value '%s' inside a function is rejected; pass REF %s to make mutation visible in the signature", target->name, target->name);
    }
    if (!receiver_sym) {
        TypeInfo *info = type_find(ctx->types, target->name);
        if (info && type_has_complete_named_value(ctx->types, info)) {
            die_at(target->line, target->col, "direct mutation of top-level named value '%s' inside a function is rejected; pass REF %s to make mutation visible in the signature", target->name, target->name);
        }
    }
}

// Extracts the payload type of a collection-like value, which lets shared join logic handle
// arrays, maps, structs, and open data because those operations have similar shape rules.
static char *collection_value_type(char *type) {
    return is_ref_type(type) ? ref_target_type(type) : type;
}

// Checks whether a type can join structurally, which protects plus from scalar misuse because
// collection joins are a separate semantic family.
static bool is_collection_join_type(const char *type) {
    return is_array_type(type) || is_map_type(type) || is_struct_type(type) || is_open_data_type(type);
}

// Analyzes in-place collection plus statements, which validates the same join rules as value plus
// because mutation changes storage, not type semantics.
static char *analyze_mutating_collection_plus_stmt(Node *expr, Scope *s, AnalyzeCtx *ctx) {
    Node *target = expr->left->expr;
    reject_direct_global_mutation_target(target, s, ctx);

    char *target_type = analyze_expr(target, s, ctx);
    char *collection_type = collection_value_type(target_type);
    if (!is_collection_join_type(collection_type)) {
        die_at(expr->line, expr->col, "mutation operator with '+' expects ARR or MAP target, got %s", target_type);
    }
    if (is_ref_type(target_type) && is_struct_type(collection_type)) {
        die_at(expr->line, expr->col, "mutating '+' on REF named MAP values is not implemented yet; mutate a field or assign a new value through the caller");
    }

    char *result_type = analyze_collection_plus_right(expr, collection_type, target, expr->right, s, ctx);
    if (target->kind == NK_NAME) {
        refine_assignment_target(target, s, result_type);
        if (is_map_type(result_type) || is_struct_type(result_type)) {
            Symbol *target_sym = scope_resolve(s, target->name, target);
            symbol_add_source_literal_keys_from_expr(target_sym, expr->right, s, ctx->types);
        }
        symbol_mark_owned_if_freeable(scope_resolve(s, target->name, target));
    }

    if (!type_eq(collection_value_type(target->checked_type), result_type)) {
        die_at(expr->line, expr->col, "mutating '+' cannot assign %s to %s", result_type, collection_type);
    }

    return set_type(expr, "NUL");
}

// Analyzes starred add/upd/filt calls, which keeps mutating methods type-compatible because
// in-place operations must preserve the receiver shape.
static char *analyze_mutating_collection_method_stmt(Node *expr, Scope *s, AnalyzeCtx *ctx) {
    Node *call = expr->expr;

    if (strcmp(call->name, "agg") == 0) {
        die_at(call->line, call->col, "mutation operator does not apply to .agg(); use .agg() as a read-only expression");
    }
    if (strcmp(call->name, "upd") != 0 && strcmp(call->name, "filt") != 0 && strcmp(call->name, "add") != 0) {
        die_at(call->line, call->col, "mutation operator only supports .add(), .upd(), and .filt() collection methods");
    }

    reject_direct_global_mutation_target(call->object, s, ctx);

    bool named_receiver = call->object && call->object->kind == NK_NAME;
    Symbol *receiver_sym = named_receiver ? scope_try_resolve(s, call->object->name) : NULL;
    bool ref_receiver = receiver_sym && is_ref_type(receiver_sym->type);
    char *receiver_type = analyze_expr(call->object, s, ctx);
    char *collection_type = collection_value_type(receiver_type);

    if (type_eq(collection_type, "STR")) {
        die_at(call->line, call->col, "in-place STR.%s() is not supported without copying; use copy-producing .%s()", call->name, call->name);
    }
    if (!is_array_type(collection_type)) {
        die_at(call->line, call->col, "in-place .%s() expects ARR[T] or REF[ARR[T]], got %s", call->name, receiver_type);
    }

    if (strcmp(call->name, "add") == 0) {
        char *result_type = analyze_array_add_result(call, collection_type, s, ctx);
        if (named_receiver && !ref_receiver) {
            refine_assignment_target(call->object, s, result_type);
            symbol_mark_owned_if_freeable(receiver_sym ? receiver_sym : scope_resolve(s, call->object->name, call->object));
        }
        return set_type(expr, "NUL");
    }

    if (!named_receiver) {
        die_at(call->line, call->col, "in-place .%s() currently requires a named array receiver", call->name);
    }

    char *element_type = array_elem_type(collection_type);
    Scope *method_scope = collection_method_scope(call, s, element_type, NULL, ctx);

    if (strcmp(call->name, "upd") == 0) {
        char *item_type = NULL;
        if (call->body) {
            if (call->args.len != 0) die_at(call->line, call->col, "*.upd() cannot mix expression arguments and method body");
            item_type = analyze_method_body(call, method_scope, ctx);
        } else {
            if (call->args.len != 1) die_at(call->line, call->col, "*.upd() expects one expression or a method body");
            item_type = analyze_expr(call->args.items[0], method_scope, ctx);
        }
        if (!type_eq(item_type, element_type)) {
            die_at(call->line, call->col, "in-place .upd() must keep element type %s, got %s", element_type, item_type);
        }
    } else {
        char *predicate_type = NULL;
        if (call->body) {
            if (call->args.len != 0) die_at(call->line, call->col, "*.filt() cannot mix expression arguments and method body");
            predicate_type = analyze_method_body(call, method_scope, ctx);
        } else {
            if (call->args.len != 1) die_at(call->line, call->col, "*.filt() expects one BOL expression or a method body");
            predicate_type = analyze_expr(call->args.items[0], method_scope, ctx);
        }
        if (!type_eq(predicate_type, "BOL")) die_at(call->line, call->col, "in-place .filt() predicate must be BOL, got %s", predicate_type);
    }

    set_type(call, "NUL");
    return set_type(expr, "NUL");
}

// Analyzes dynamic MAP literals, which keeps runtime-key objects typed as OPEN because their keys
// cannot be closed into fixed C fields.
static char *analyze_open_map_literal(Node *literal, Scope *s, AnalyzeCtx *ctx, const char *type) {
    for (int i = 0; i < literal->fields.len; i++) {
        Node *entry = literal->fields.items[i];
        set_type(entry, "NUL");

        char *entry_key_type = analyze_expr_expected(entry->target, s, ctx, "STR");
        if (!type_eq(entry_key_type, "STR")) {
            die_at(entry->target->line, entry->target->col, "open MAP literal keys currently must be STR, got %s", entry_key_type);
        }

        analyze_expr(entry->value, s, ctx);
    }

    return set_type(literal, type);
}

// Analyzes the right side of a join, which decides whether fields, keys, or elements are being
// overlaid because + is overloaded by collection shape.
static char *analyze_join_right(Node *at, const char *left_type, Node *right, Scope *s, AnalyzeCtx *ctx) {
    if (is_array_type(left_type)) {
        char *element_type = array_elem_type(left_type);

        if (right->kind == NK_ARRAY) {
            if (is_array_type(element_type) &&
                (right->elements.len == 0 || ((Node *)right->elements.items[0])->kind != NK_ARRAY)) {
                char *right_type = analyze_expr_expected(right, s, ctx, element_type);
                if (!type_eq(right_type, element_type)) die_at(right->line, right->col, "array append needs %s, got %s", element_type, right_type);
                return xstrdup(left_type);
            }

            char *right_type = is_unknown_array_type(left_type)
                ? analyze_expr(right, s, ctx)
                : analyze_expr_expected(right, s, ctx, left_type);
            if (is_unknown_array_type(left_type) && is_array_type(right_type) && !is_unknown_array_type(right_type)) return right_type;
            if (!type_eq(right_type, left_type)) die_at(right->line, right->col, "array join needs %s, got %s", left_type, right_type);
            return xstrdup(left_type);
        }

        char *value_type = is_unknown_type(element_type)
            ? analyze_expr(right, s, ctx)
            : analyze_expr_expected(right, s, ctx, element_type);

        if (is_unknown_type(element_type)) return array_type(value_type);
        if (!type_eq(value_type, element_type)) die_at(right->line, right->col, "array append needs %s, got %s", element_type, value_type);
        return xstrdup(left_type);
    }

    if (is_map_type(left_type)) {
        char *right_type = analyze_expr_expected(right, s, ctx, left_type);
        if (!type_eq(right_type, left_type)) die_at(right->line, right->col, "join needs %s, got %s", left_type, right_type);
        return xstrdup(left_type);
    }

    if (is_struct_type(left_type)) {
        if (right->kind == NK_MAP_LITERAL) {
            mark_struct_open(ctx->types, left_type);
            analyze_open_map_literal(right, s, ctx, left_type);
            return xstrdup(left_type);
        }

        char *right_type = analyze_expr_expected(right, s, ctx, left_type);
        if (!type_eq(right_type, left_type)) die_at(right->line, right->col, "join needs %s, got %s", left_type, right_type);
        return xstrdup(left_type);
    }

    die_at(at->line, at->col, "+= join is not implemented for %s", left_type);
    return NULL;
}

// Analyzes array add, which distinguishes append from extend because foo.add(5) and foo.add([5,
// 6]) intentionally share one method.
static char *analyze_array_add_result(Node *call, const char *object_type, Scope *s, AnalyzeCtx *ctx) {
    if (call->body) die_at(call->line, call->col, ".add() does not take a method body");
    if (call->args.len != 1) die_at(call->line, call->col, ".add() expects one value or one ARR value");

    Node *arg = call->args.items[0];
    char *element_type = is_unknown_array_type(object_type) ? xstrdup("?") : array_elem_type(object_type);
    char *arg_type = NULL;

    if (!is_unknown_array_type(object_type) && arg->kind == NK_RECORD_LITERAL && is_struct_type(element_type)) {
        arg_type = analyze_expr_expected(arg, s, ctx, element_type);
    } else if (!is_unknown_array_type(object_type) && arg->kind == NK_ARRAY && !is_array_type(element_type)) {
        arg_type = analyze_expr_expected(arg, s, ctx, object_type);
    } else {
        arg_type = analyze_expr(arg, s, ctx);
    }

    if (is_unknown_array_type(object_type)) {
        if (is_unknown_array_type(arg_type)) {
            die_at(arg->line, arg->col, ".add([]) needs a typed receiver");
        }
        char *result_type = (is_array_type(arg_type) && arg->kind != NK_ARRAY)
            ? array_type(arg_type)
            : (is_array_type(arg_type) ? xstrdup(arg_type) : array_type(arg_type));
        set_type(call->object, result_type);
        if (call->object->kind == NK_NAME) {
            Symbol *sym = scope_try_resolve(s, call->object->name);
            if (sym) refine_symbol_type(sym, result_type);
        }
        return set_type(call, result_type);
    }

    if (type_eq(arg_type, object_type)) return set_type(call, object_type);

    if (type_conforms_to_expected(arg_type, element_type) ||
        type_assignable_to_expected(arg_type, element_type)) {
        if ((arg->kind == NK_RECORD_LITERAL && is_struct_type(element_type)) ||
            (arg->kind == NK_ARRAY && is_array_type(element_type) && !type_eq(arg_type, element_type))) {
            arg_type = analyze_expr_expected(arg, s, ctx, element_type);
        }
        if (!type_conforms_to_expected(arg_type, element_type) &&
            !type_assignable_to_expected(arg_type, element_type)) {
            die_at(arg->line, arg->col, ".add() value must be %s or %s, got %s", element_type, object_type, arg_type);
        }
        return set_type(call, object_type);
    }

    die_at(arg->line, arg->col, ".add() value must be %s or %s, got %s", element_type, object_type, arg_type);
    return NULL;
}

// Analyzes copy-producing collection plus, which computes the resulting structural type because
// joins may widen open maps or merge fixed fields.
static char *analyze_collection_plus_right(Node *at, const char *left_type, Node *left, Node *right, Scope *s, AnalyzeCtx *ctx) {
    if (is_array_type(left_type)) {
        char *right_type = NULL;

        if (is_unknown_array_type(left_type)) {
            right_type = analyze_expr(right, s, ctx);
            if (!is_array_type(right_type) || is_unknown_array_type(right_type)) {
                die_at(right->line, right->col, "array '+' needs a typed ARR right side, got %s", right_type);
            }
            if (left) {
                set_type(left, right_type);
                if (left->kind == NK_NAME) {
                    Symbol *sym = scope_try_resolve(s, left->name);
                    if (sym) refine_symbol_type(sym, right_type);
                }
            }
            return xstrdup(right_type);
        }

        right_type = analyze_expr_expected(right, s, ctx, left_type);
        if (!type_eq(right_type, left_type)) {
            die_at(right->line, right->col, "array '+' needs %s, got %s", left_type, right_type);
        }
        return xstrdup(left_type);
    }

    if (is_map_type(left_type) || is_struct_type(left_type)) {
        return analyze_join_right(at, left_type, right, s, ctx);
    }

    if (is_open_data_type(left_type)) {
        char *right_type = analyze_expr_expected(right, s, ctx, "OPEN");
        if (!is_open_data_type(right_type)) die_at(right->line, right->col, "open MAP '+' needs OPEN, got %s", right_type);
        return xstrdup("OPEN");
    }

    die_at(at->line, at->col, "operator '+' is not implemented for %s", left_type);
    return NULL;
}

// Refines an assignment target after inference, which lets empty literals and open structures
// become concrete because the first meaningful assignment supplies their type.
static void refine_assignment_target(Node *target, Scope *s, const char *type) {
    if (target->kind != NK_NAME) return;
    Symbol *sym = scope_resolve(s, target->name, target);
    if (is_unknown_array_type(sym->type) && is_array_type(type) && !is_unknown_array_type(type)) {
        refine_symbol_type(sym, type);
        set_type(target, type);
    }
}
