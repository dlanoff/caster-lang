// Analyzer top-level program, named value, and function body orchestration.

// Analyzes a function body, which checks parameters, returns, ownership, and nested helpers
// because function signatures are Caster's typed edges.
static void analyze_fn(FnInfo *fn, FnTable *fns, TypeTable *types, Scope *global_scope) {
    if (fn->state == 2) return;
    if (fn->state == 1) {
        if (fn->declared_return_type) {
            fn->return_type = xstrdup(fn->declared_return_type);
            return;
        }
        die_at(fn->node->line, fn->node->col, "recursive return inference for function '%s' needs an explicit -> return type", fn->name);
    }

    fn->state = 1;

    Scope *scope = scope_new(global_scope);
    for (int i = 0; i < fn->param_count; i++) {
        Node *param = fn->node->params.items[i];
        Symbol *sym = scope_define(scope, param->name, fn->param_types[i], NULL, param);
        sym->is_param = true;
        sym->ownership = OWNERSHIP_BORROWED;
        set_type(param, fn->param_types[i]);
    }

    AnalyzeCtx ctx = {
        .fn = fn,
        .fns = fns,
        .types = types,
        .global_scope = global_scope,
        .return_type = NULL,
        .saw_return = false,
        .enforce_local_style = true,
        .allow_json_decode_boundary = true,
        .allow_terminal_tap = false,
        .next_loop_id = 1,
        .loop_depth = 0
    };
    analyze_block(fn->node->body, scope, &ctx);
    validate_concrete_types(fn->node->body);

    if (fn->declared_return_type) {
        if (!ctx.saw_return && !type_eq(fn->declared_return_type, "NUL")) {
            die_at(fn->node->line, fn->node->col, "function '%s' declares -> %s but has no ret", fn->name, fn->declared_return_type);
        }
        fn->return_type = xstrdup(fn->declared_return_type);
    } else {
        fn->return_type = ctx.saw_return ? ctx.return_type : xstrdup("NUL");
    }

    set_type(fn->node, fn->return_type);
    fn->state = 2;
}

// Analyzes named MAP defaults, which validates INIT and field defaults before functions because
// named values behave like typed prototypes for later use.
static void analyze_named_map_values(TypeTable *types, Scope *global_scope) {
    AnalyzeCtx ctx = {0};
    ctx.types = types;
    ctx.global_scope = global_scope;
    ctx.next_loop_id = 1;

    for (int i = 0; i < types->len; i++) {
        TypeInfo *info = &types->items[i];

        if (info->kind == TYPEINFO_ALIAS) {
            if (info->node->value) {
                char *value_type = analyze_expr_expected(info->node->value, global_scope, &ctx, info->target);
                if (!type_assignable_to_expected(value_type, info->target)) {
                    die_at(info->node->value->line, info->node->value->col, "named MAP value %s must be %s, got %s", info->name, info->target, value_type);
                }
            }
            continue;
        }

        for (int j = 0; j < info->node->fields.len; j++) {
            Node *field = info->node->fields.items[j];
            if (!field->value) {
                TypeInfo *field_info = field->text ? type_find(types, field->text) : NULL;
                if (field_info && type_has_complete_named_value(types, field_info)) {
                    die_at(field->line, field->col, "field '%s' uses named MAP value '%s'; write '%s %s = INIT' for explicit nested initialization", field->name, field->text, field->text, field->name);
                }
                continue;
            }
            if (field->value->kind == NK_INIT) {
                TypeInfo *field_info = field->text ? type_find(types, field->text) : NULL;
                if (!field_info) {
                    die_at(field->value->line, field->value->col, "INIT requires a named MAP value field");
                }
                char *init_type = field_info->kind == TYPEINFO_STRUCT ? field_info->name : field_info->target;
                if (!type_eq(init_type, field->declared_type)) {
                    die_at(field->value->line, field->value->col, "INIT for field '%s' must be %s, got %s", field->name, field->declared_type, init_type);
                }
                if (!type_has_complete_named_value(types, field_info)) {
                    die_at(field->value->line, field->value->col, "INIT source '%s' has no complete named value", field->text);
                }
                set_type(field->value, field->declared_type);
                continue;
            }
            char *value_type = analyze_expr_expected(field->value, global_scope, &ctx, field->declared_type);
            if (!type_assignable_to_expected(value_type, field->declared_type)) {
                die_at(field->value->line, field->value->col, "default for field '%s' must be %s, got %s", field->name, field->declared_type, value_type);
            }
        }
    }
}

// Analyzes the whole program, which orders types, globals, functions, nested helpers, and scripts
// because later phases assume every node already has a checked type.
static void analyze_program(Node *prog) {
    memset(&g_type_storage, 0, sizeof(g_type_storage));
    build_type_table(prog, &g_type_storage);
    TypeTable *types = &g_type_storage;
    g_types = types;

    Scope *global_scope = scope_new(NULL);
    analyze_named_map_values(types, global_scope);

    for (int i = 0; i < prog->globals.len; i++) {
        Node *global = prog->globals.items[i];
        char *declared = resolve_type(types, global->declared_type, global);
        global->declared_type = declared;
        if (global->value) {
            AnalyzeCtx global_ctx = {
                .fn = NULL,
                .fns = NULL,
                .types = types,
                .global_scope = global_scope,
                .return_type = NULL,
                .saw_return = false,
                .enforce_local_style = false,
                .allow_json_decode_boundary = false,
                .allow_terminal_tap = false,
                .next_loop_id = 1,
                .loop_depth = 0
            };
            char *value_type = analyze_expr_expected(global->value, global_scope, &global_ctx, declared);
            if (!type_assignable_to_expected(value_type, declared)) {
                die_at(global->value->line, global->value->col, "cannot initialize global %s as %s with %s", global->name, declared, value_type);
            }
        }
        global->c_expr = strf("global_%s", global->name);
        Symbol *sym = scope_define(global_scope, global->name, declared, global->c_expr, global);
        sym->is_global = true;
        sym->ownership = OWNERSHIP_BORROWED;
        set_type(global, "NUL");
    }

    FnTable table = {0};
    for (int i = 0; i < prog->functions.len; i++) {
        Node *fn = prog->functions.items[i];
        register_function_info(&table, types, fn, fn->name);
    }

    for (int i = 0; i < prog->functions.len; i++) {
        Node *fn = prog->functions.items[i];
        register_nested_functions_in_block(&table, types, fn->body, fn->name);
    }
    if (prog->statements.len) register_nested_functions_in_block(&table, types, prog, "script");

    FnInfo *mainfn = fn_find(&table, "main");
    if (mainfn && mainfn->param_count != 0) die_at(mainfn->node->line, mainfn->node->col, "FN main() cannot take parameters yet");
    if (mainfn && prog->statements.len) die_at(mainfn->node->line, mainfn->node->col, "cannot combine FN main() with top-level script statements");

    for (int i = 0; i < table.len; i++) analyze_fn(&table.items[i], &table, types, global_scope);

    if (prog->statements.len) {
        FnInfo script_fn = {0};
        script_fn.name = "main";
        script_fn.declared_return_type = "INT";
        script_fn.return_type = "INT";

        AnalyzeCtx script_ctx = {
            .fn = &script_fn,
            .fns = &table,
            .types = types,
            .global_scope = global_scope,
            .return_type = NULL,
            .saw_return = false,
            .enforce_local_style = true,
            .allow_json_decode_boundary = true,
            .allow_terminal_tap = false,
            .next_loop_id = 1,
            .loop_depth = 0
        };
        analyze_block(prog, global_scope, &script_ctx);
        validate_concrete_types(prog);
    } else {
        set_type(prog, "NUL");
    }
}
