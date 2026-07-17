// Analyzer function, native adapter, system adapter, and method call checks.

// Analyzes a fixed MAP constructor call, which turns TypeName(value) into an explicit
// structural conformance boundary because the call names the intended closed shape.
static char *analyze_struct_constructor_call(Node *call, Scope *s, AnalyzeCtx *ctx, const char *type_name, const char *display_name) {
    TypeInfo *info = type_find(ctx->types, type_name);
    if (!info || info->kind != TYPEINFO_STRUCT) return NULL;
    if (call->args.len != 1 || call->body) {
        die_at(call->line, call->col, "%s(value) expects exactly one value", display_name);
    }

    AnalyzeCtx arg_ctx = *ctx;
    arg_ctx.allow_json_decode_boundary = true;
    char *arg_type = analyze_expr_expected(call->args.items[0], s, &arg_ctx, info->name);
    ctx->next_loop_id = arg_ctx.next_loop_id;
    if (!type_assignable_to_expected(arg_type, info->name) && !type_conforms_to_expected(arg_type, info->name)) {
        Node *arg = call->args.items[0];
        die_at(arg->line, arg->col, "%s(value) expects %s, got %s", display_name, info->name, arg_type);
    }

    call->declared_type = xstrdup(info->name);
    return set_type(call, info->name);
}

// Analyzes function calls, which turns callee and argument syntax into a checked return type
// because calls are the main boundary where local inference meets explicit signatures.
static char *analyze_call(Node *call, Scope *s, AnalyzeCtx *ctx) {
    if (call->callee->kind != NK_NAME) {
        char *callee_type = analyze_expr(call->callee, s, ctx);
        if (!is_fn_type(callee_type)) {
            die_at(call->line, call->col, "call target must be a function, got %s", callee_type);
        }
        if (call->args.len != 1) die_at(call->line, call->col, "function value expects one argument");
        char *input = fn_input_type(callee_type);
        char *output = fn_output_type(callee_type);
        char *arg_type = analyze_expr_expected(call->args.items[0], s, ctx, input);
        if (!type_eq(arg_type, input)) {
            Node *arg = call->args.items[0];
            die_at(arg->line, arg->col, "function value argument must be %s, got %s", input, arg_type);
        }
        return set_type(call, output);
    }

    const char *name = call->callee->name;
    if (strcmp(name, "RAND") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "RAND(scale) expects one INT or FLT scale");
        char *scale_type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(scale_type, "INT") && !type_eq(scale_type, "FLT")) {
            Node *arg = call->args.items[0];
            die_at(arg->line, arg->col, "RAND(scale) expects INT or FLT scale, got %s", scale_type);
        }
        return set_type(call, "FLT");
    }

    if (is_cast_call_name(name)) {
        if (strcmp(name, "ARR") == 0) {
            if (call->args.len != 1 && call->args.len != 2) die_at(call->line, call->col, "ARR() expects text, or text and separator");
            char *text_type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(text_type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "ARR() expects STR text, got %s", text_type);
            if (call->args.len == 2) {
                char *sep_type = analyze_expr(call->args.items[1], s, ctx);
                if (!type_eq(sep_type, "STR")) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "ARR(text, separator) expects STR separator, got %s", sep_type);
            }
            return set_type(call, "ARR[STR]");
        }

        if (strcmp(name, "STR") == 0 && call->args.len == 2) {
            char *source_type = analyze_expr(call->args.items[0], s, ctx);
            char *delimiter_type = analyze_expr(call->args.items[1], s, ctx);
            if (!is_str_array_type(source_type)) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "STR(value, delimiter) expects ARR[STR], got %s", source_type);
            if (!type_eq(delimiter_type, "STR")) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "STR(value, delimiter) expects STR delimiter, got %s", delimiter_type);
            return set_type(call, "STR");
        }

        if (call->args.len != 1) die_at(call->line, call->col, "%s() expects exactly one argument", name);
        char *source_type = analyze_expr(call->args.items[0], s, ctx);
        if (strcmp(name, "STR") == 0 && is_str_array_type(source_type)) return set_type(call, "STR");
        if (!is_cast_source_type(source_type)) {
            Node *arg = call->args.items[0];
            die_at(arg->line, arg->col, "%s() supports INT, FLT, BOL, STR%s, got %s", name, strcmp(name, "STR") == 0 ? ", or ARR[STR]" : "", source_type);
        }
        return set_type(call, name);
    }

    char *constructor_type = analyze_struct_constructor_call(call, s, ctx, name, name);
    if (constructor_type) return constructor_type;

    Symbol *callee_sym = scope_try_resolve(s, name);
    if (callee_sym && callee_sym->node && callee_sym->node->kind == NK_FN && callee_sym->c_expr) {
        FnInfo *fn = fn_find(ctx->fns, callee_sym->c_expr);
        if (!fn) die_at(call->callee->line, call->callee->col, "unknown nested function '%s'", name);
        if (call->args.len != fn->param_count) die_at(call->line, call->col, "function '%s' expects %d argument(s), got %d", name, fn->param_count, call->args.len);

        for (int i = 0; i < call->args.len; i++) {
            Node *arg = call->args.items[i];
            char *type = analyze_expr_expected(arg, s, ctx, fn->param_types[i]);
            if (!type_eq(type, fn->param_types[i])) {
                die_at(arg->line, arg->col, "argument %d to '%s' must be %s, got %s", i + 1, name, fn->param_types[i], type);
            }
        }

        if (fn->state != 2) analyze_fn(fn, ctx->fns, ctx->types, ctx->global_scope);
        call->callee->c_expr = xstrdup(callee_sym->c_expr);
        set_type(call->callee, "NUL");
        if (fn->returns_owned_ref && is_ref_type(fn->return_type)) call->ref_result_owned = true;
        return set_type(call, fn->return_type);
    }

    if (callee_sym && is_fn_type(callee_sym->type)) {
        if (call->args.len != 1) die_at(call->line, call->col, "function value expects one argument");
        char *input = fn_input_type(callee_sym->type);
        char *output = fn_output_type(callee_sym->type);
        char *arg_type = analyze_expr_expected(call->args.items[0], s, ctx, input);
        if (!type_eq(arg_type, input)) {
            Node *arg = call->args.items[0];
            die_at(arg->line, arg->col, "function value argument must be %s, got %s", input, arg_type);
        }
        call->callee->c_expr = callee_sym->c_expr ? xstrdup(callee_sym->c_expr) : NULL;
        set_type(call->callee, callee_sym->type);
        return set_type(call, output);
    }

    if (strcmp(name, "log") == 0) {
        if (call->args.len < 1) die_at(call->line, call->col, "log expects at least one argument");
        for (int i = 0; i < call->args.len; i++) {
            Node *arg = call->args.items[i];
            char *type = analyze_expr(arg, s, ctx);
            if (!type_eq(type, "INT") && !type_eq(type, "FLT") && !type_eq(type, "BOL") && !type_eq(type, "STR") && !type_eq(type, "OBJ")) {
                die_at(arg->line, arg->col, "log does not support %s", type);
            }
        }
        return set_type(call, "NUL");
    }

    if (strcmp(name, "assert") == 0) {
        if (call->args.len < 1 || call->args.len > 2) {
            die_at(call->line, call->col, "assert expects a BOL condition and optional STR message");
        }

        char *condition_type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(condition_type, "BOL")) {
            Node *arg = call->args.items[0];
            die_at(arg->line, arg->col, "assert condition must be BOL, got %s", condition_type);
        }

        if (call->args.len == 2) {
            char *message_type = analyze_expr(call->args.items[1], s, ctx);
            if (!type_eq(message_type, "STR")) {
                Node *arg = call->args.items[1];
                die_at(arg->line, arg->col, "assert message must be STR, got %s", message_type);
            }
        }

        return set_type(call, "NUL");
    }

    if (strcmp(name, "HEAP") == 0) {
        die_at(call->line, call->col, "HEAP is no longer user-facing; create a value with 'name = Type' and use 'REF name'");
    }

    if (strcmp(name, "len") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "len expects exactly one argument");
        char *type = analyze_expr(call->args.items[0], s, ctx);
        if (!is_array_type(type) && !type_eq(type, "STR") && !is_map_type(type) && !is_open_data_type(type) && !is_open_struct_type(type)) {
            die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "len expects ARR, STR, or MAP, got %s", type);
        }
        return set_type(call, "INT");
    }

    if (strcmp(name, "has") == 0) {
        if (call->args.len != 2) die_at(call->line, call->col, "has expects a collection and a value");
        char *collection_type = analyze_expr(call->args.items[0], s, ctx);
        char *needle_type = analyze_expr(call->args.items[1], s, ctx);
        if (is_array_type(collection_type)) {
            char *element_type = array_elem_type(collection_type);
            if (!type_eq(needle_type, element_type)) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "has needle must be %s, got %s", element_type, needle_type);
            return set_type(call, "BOL");
        }
        if (type_eq(collection_type, "STR")) {
            if (!type_eq(needle_type, "STR")) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "has on STR expects STR needle, got %s", needle_type);
            return set_type(call, "BOL");
        }
        if (is_map_type(collection_type)) {
            char *key_type = map_key_type(collection_type);
            if (!type_eq(needle_type, key_type)) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "has on MAP expects %s key, got %s", key_type, needle_type);
            return set_type(call, "BOL");
        }
        if (is_open_data_type(collection_type) || is_open_struct_type(collection_type) || type_eq(collection_type, "OBJ")) {
            if (!type_eq(needle_type, "STR")) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "has on open MAP expects STR key, got %s", needle_type);
            return set_type(call, "BOL");
        }
        die_at(call->line, call->col, "has expects ARR, STR, or MAP, got %s", collection_type);
    }

    if (strcmp(name, "find") == 0) {
        if (call->args.len != 2) die_at(call->line, call->col, "find expects a collection and a value");
        char *collection_type = analyze_expr(call->args.items[0], s, ctx);
        char *needle_type = analyze_expr(call->args.items[1], s, ctx);
        if (is_array_type(collection_type)) {
            char *element_type = array_elem_type(collection_type);
            if (!type_eq(needle_type, element_type)) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "find needle must be %s, got %s", element_type, needle_type);
            return set_type(call, "INT");
        }
        if (type_eq(collection_type, "STR")) {
            if (!type_eq(needle_type, "STR")) die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "find on STR expects STR needle, got %s", needle_type);
            return set_type(call, "INT");
        }
        die_at(call->line, call->col, "find expects ARR or STR, got %s", collection_type);
    }

    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "%s expects exactly one collection", name);
        char *collection_type = analyze_expr(call->args.items[0], s, ctx);
        if (is_array_type(collection_type)) {
            char *element_type = array_elem_type(collection_type);
            if (!type_eq(element_type, "INT") && !type_eq(element_type, "FLT") && !type_eq(element_type, "STR")) {
                die_at(call->line, call->col, "%s currently supports ARR[INT], ARR[FLT], or ARR[STR], got %s", name, collection_type);
            }
            return set_type(call, element_type);
        }
        if (type_eq(collection_type, "STR")) return set_type(call, "STR");
        die_at(call->line, call->col, "%s expects ARR or STR, got %s", name, collection_type);
    }

    if (strcmp(name, "trim") == 0 || strcmp(name, "lower") == 0 || strcmp(name, "upper") == 0 || strcmp(name, "urlDecode") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "%s expects one STR", name);
        char *text_type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(text_type, "STR")) die_at(call->line, call->col, "%s expects STR, got %s", name, text_type);
        return set_type(call, "STR");
    }

    if (strcmp(name, "replace") == 0) {
        if (call->args.len != 3) die_at(call->line, call->col, "replace expects text, old, next");
        for (int i = 0; i < 3; i++) {
            char *type = analyze_expr(call->args.items[i], s, ctx);
            if (!type_eq(type, "STR")) die_at(((Node *)call->args.items[i])->line, ((Node *)call->args.items[i])->col, "replace arguments must be STR, got %s", type);
        }
        return set_type(call, "STR");
    }

    if (strcmp(name, "starts") == 0 || strcmp(name, "ends") == 0) {
        if (call->args.len != 2) die_at(call->line, call->col, "%s expects text and affix", name);
        for (int i = 0; i < 2; i++) {
            char *type = analyze_expr(call->args.items[i], s, ctx);
            if (!type_eq(type, "STR")) die_at(((Node *)call->args.items[i])->line, ((Node *)call->args.items[i])->col, "%s arguments must be STR, got %s", name, type);
        }
        return set_type(call, "BOL");
    }

    if (strcmp(name, "isDigit") == 0 || strcmp(name, "isAlpha") == 0 || strcmp(name, "isSpace") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "%s expects one STR", name);
        char *type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "%s expects STR, got %s", name, type);
        return set_type(call, "BOL");
    }

    if (strcmp(name, "JSON") == 0) {
        if (call->args.len != 1) die_at(call->line, call->col, "JSON expects exactly one STR argument");
        char *type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "JSON expects STR, got %s", type);
        return set_type(call, "OBJ");
    }

    FnInfo *fn = fn_find(ctx->fns, name);
    if (!fn) die_at(call->callee->line, call->callee->col, "unknown function '%s'", name);
    if (call->args.len != fn->param_count) die_at(call->line, call->col, "function '%s' expects %d argument(s), got %d", name, fn->param_count, call->args.len);

    for (int i = 0; i < call->args.len; i++) {
        Node *arg = call->args.items[i];
        char *type = analyze_expr_expected(arg, s, ctx, fn->param_types[i]);
        if (!type_eq(type, fn->param_types[i])) {
            die_at(arg->line, arg->col, "argument %d to '%s' must be %s, got %s", i + 1, name, fn->param_types[i], type);
        }
    }

    if (fn->state != 2) analyze_fn(fn, ctx->fns, ctx->types, ctx->global_scope);
    if (fn->returns_owned_ref && is_ref_type(fn->return_type)) call->ref_result_owned = true;
    return set_type(call, fn->return_type);
}

// Checks a method receiver name, which keeps native module dispatch compact because adapter calls
// are represented as ordinary dot syntax in the AST.
static bool method_object_is(Node *call, const char *name) {
    return call->object && call->object->kind == NK_NAME && strcmp(call->object->name, name) == 0;
}

// Validates adapter arity, which keeps native call diagnostics near the adapter table because
// those calls bypass user-defined function lookup.
static void require_arg_count(Node *call, int expected) {
    if (call->args.len != expected) {
        die_at(call->line, call->col, "%s.%s() expects %d argument(s), got %d",
            call->object->name, call->name, expected, call->args.len);
    }
}

// Validates adapter argument types, which keeps native APIs strict because their C entry points do
// not have Caster signatures to recheck them later.
static void require_arg_type(Node *call, Scope *s, AnalyzeCtx *ctx, int index, const char *expected) {
    Node *arg = call->args.items[index];
    char *type = analyze_expr_expected(arg, s, ctx, expected);
    if (!type_eq(type, expected)) {
        die_at(arg->line, arg->col, "argument %d to %s.%s() must be %s, got %s",
            index + 1, call->object->name, call->name, expected, type);
    }
}

// Analyzes OS/FS/PATH/IO/PROC calls, which centralizes system adapter typing because these modules
// share the same runtime implementation file.
static char *analyze_system_adapter_call(Node *call, Scope *s, AnalyzeCtx *ctx) {
    if (method_object_is(call, "OS")) {
        if (strcmp(call->name, "args") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "ARR[STR]");
        }
        if (strcmp(call->name, "exit") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "INT");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "cwd") == 0 || strcmp(call->name, "platform") == 0 || strcmp(call->name, "arch") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }
        if (strcmp(call->name, "chdir") == 0 || strcmp(call->name, "env") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, strcmp(call->name, "env") == 0 ? "STR" : "NUL");
        }
        if (strcmp(call->name, "setEnv") == 0) {
            require_arg_count(call, 2);
            require_arg_type(call, s, ctx, 0, "STR");
            require_arg_type(call, s, ctx, 1, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "isWindows") == 0 || strcmp(call->name, "isMac") == 0 || strcmp(call->name, "isLinux") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "BOL");
        }
        die_at(call->line, call->col, "OS.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "FS")) {
        if (strcmp(call->name, "read") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }
        if (strcmp(call->name, "readBytes") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "ARR[INT]");
        }
        if (strcmp(call->name, "write") == 0 || strcmp(call->name, "append") == 0) {
            require_arg_count(call, 2);
            require_arg_type(call, s, ctx, 0, "STR");
            require_arg_type(call, s, ctx, 1, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "exists") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "BOL");
        }
        if (strcmp(call->name, "remove") == 0 || strcmp(call->name, "mkdir") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "rename") == 0 || strcmp(call->name, "copy") == 0) {
            require_arg_count(call, 2);
            require_arg_type(call, s, ctx, 0, "STR");
            require_arg_type(call, s, ctx, 1, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "list") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "ARR[STR]");
        }
        if (strcmp(call->name, "stat") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "FileStat");
        }
        die_at(call->line, call->col, "FS.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "PATH")) {
        if (strcmp(call->name, "join") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "ARR[STR]");
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }
        if (strcmp(call->name, "parent") == 0 || strcmp(call->name, "name") == 0 ||
            strcmp(call->name, "ext") == 0 || strcmp(call->name, "absolute") == 0 ||
            strcmp(call->name, "normalize") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }
        die_at(call->line, call->col, "PATH.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "IO")) {
        if (strcmp(call->name, "print") == 0 || strcmp(call->name, "error") == 0) {
            require_arg_count(call, 1);
            char *type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(type, "INT") && !type_eq(type, "FLT") && !type_eq(type, "BOL") && !type_eq(type, "STR")) {
                die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "IO.%s() supports INT, FLT, BOL, or STR, got %s", call->name, type);
            }
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }
        if (strcmp(call->name, "readLine") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }
        if (strcmp(call->name, "stdin") == 0 || strcmp(call->name, "stdout") == 0 || strcmp(call->name, "stderr") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "INT");
        }
        die_at(call->line, call->col, "IO.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "PROC")) {
        if (strcmp(call->name, "run") == 0) {
            require_arg_count(call, 2);
            require_arg_type(call, s, ctx, 0, "STR");
            require_arg_type(call, s, ctx, 1, "ARR[STR]");
            set_type(call->object, "NUL");
            return set_type(call, "ProcResult");
        }
        if (strcmp(call->name, "spawn") == 0 || strcmp(call->name, "wait") == 0) {
            die_at(call->line, call->col, "PROC.%s() is planned but not implemented yet; use PROC.run(command, args)", call->name);
        }
        die_at(call->line, call->col, "PROC.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "BUF")) {
        if (!type_find(ctx->types, "Buffer")) {
            die_at(call->line, call->col, "BUF.%s() requires `use BUF`", call->name);
        }

        if (strcmp(call->name, "new") == 0) {
            require_arg_count(call, 0);
            set_type(call->object, "NUL");
            return set_type(call, "Buffer");
        }

        if (strcmp(call->name, "toStr") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "Buffer");
            set_type(call->object, "NUL");
            return set_type(call, "STR");
        }

        if (strcmp(call->name, "len") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "Buffer");
            set_type(call->object, "NUL");
            return set_type(call, "INT");
        }

        if (strcmp(call->name, "clear") == 0 || strcmp(call->name, "free") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "REF[Buffer]");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }

        if (strcmp(call->name, "write") == 0 || strcmp(call->name, "line") == 0) {
            require_arg_count(call, 2);
            require_arg_type(call, s, ctx, 0, "REF[Buffer]");
            char *value_type = analyze_expr(call->args.items[1], s, ctx);
            if (!type_eq(value_type, "INT") && !type_eq(value_type, "FLT") && !type_eq(value_type, "BOL") && !type_eq(value_type, "STR")) {
                Node *arg = call->args.items[1];
                die_at(arg->line, arg->col, "BUF.%s() supports INT, FLT, BOL, or STR, got %s", call->name, value_type);
            }
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }

        die_at(call->line, call->col, "BUF.%s() is not implemented", call->name);
    }

    if (method_object_is(call, "SQL")) {
        if (strcmp(call->name, "open") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "STR");
            set_type(call->object, "NUL");
            return set_type(call, "SQL_DB");
        }

        if (strcmp(call->name, "close") == 0) {
            require_arg_count(call, 1);
            require_arg_type(call, s, ctx, 0, "REF[SQL_DB]");
            set_type(call->object, "NUL");
            return set_type(call, "NUL");
        }

        if (strcmp(call->name, "exec") == 0 || strcmp(call->name, "query") == 0) {
            require_arg_count(call, 3);
            require_arg_type(call, s, ctx, 0, "REF[SQL_DB]");
            require_arg_type(call, s, ctx, 1, "STR");
            Node *params = call->args.items[2];
            char *params_type = analyze_expr_expected(params, s, ctx, "OBJ");
            if (!type_eq(params_type, "OBJ")) {
                die_at(params->line, params->col, "SQL.%s() params must be a DYN array, got %s", call->name, params_type);
            }
            set_type(call->object, "NUL");
            return set_type(call, strcmp(call->name, "exec") == 0 ? "SQL_Exec" : "OBJ");
        }

        die_at(call->line, call->col, "SQL.%s() is not implemented", call->name);
    }

    return NULL;
}

// Analyzes method calls, which routes collection, string, map, DYN, and adapter methods because
// dot syntax is intentionally broad in Caster.
static char *analyze_method_call(Node *call, Scope *s, AnalyzeCtx *ctx) {
    char *system_type = analyze_system_adapter_call(call, s, ctx);
    if (system_type) return system_type;

    if (call->object && call->object->kind == NK_NAME && strcmp(call->name, "INIT") == 0) {
        if (call->args.len != 0 || call->body) {
            die_at(call->line, call->col, "%s.INIT() expects no arguments or body", call->object->name);
        }
        TypeInfo *info = type_find(ctx->types, call->object->name);
        if (!info || !type_has_complete_named_value(ctx->types, info)) {
            die_at(call->object->line, call->object->col, "unknown named value '%s' for INIT", call->object->name);
        }
        set_type(call->object, "NUL");
        call->declared_type = xstrdup(info->name);
        return set_type(call, info->kind == TYPEINFO_STRUCT ? info->name : info->target);
    }

    if (call->object && call->object->kind == NK_NAME && strcmp(call->object->name, "JSON") == 0) {
        if (strcmp(call->name, "stringify") != 0) {
            die_at(call->line, call->col, "JSON.%s() is not implemented", call->name);
        }
        if (call->args.len != 1) die_at(call->line, call->col, "JSON.stringify() expects one DYN value");
        char *value_type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(value_type, "OBJ")) {
            die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "JSON.stringify() expects DYN, got %s", value_type);
        }
        set_type(call->object, "NUL");
        return set_type(call, "STR");
    }

    if (call->object && call->object->kind == NK_NAME && strcmp(call->object->name, "REQ") == 0) {
        if (strcmp(call->name, "host") == 0 || strcmp(call->name, "hostTLS") == 0) {
            bool tls_host = strcmp(call->name, "hostTLS") == 0;
            int min_args = tls_host ? 4 : 2;
            int max_args = tls_host ? 5 : 3;
            if (call->args.len != min_args && call->args.len != max_args) {
                if (tls_host) die_at(call->line, call->col, "REQ.hostTLS() expects INT port, STR certFile, STR keyFile, and handler function, or INT port, STR certFile, STR keyFile, REF context, and handler function");
                die_at(call->line, call->col, "REQ.host() expects INT port and handler function, or INT port, REF context, and handler function");
            }
            char *port_type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(port_type, "INT")) {
                die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "REQ.%s() port must be INT, got %s", call->name, port_type);
            }
            if (tls_host) {
                char *cert_type = analyze_expr(call->args.items[1], s, ctx);
                if (!type_eq(cert_type, "STR")) {
                    die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "REQ.hostTLS() certFile must be STR, got %s", cert_type);
                }
                char *key_type = analyze_expr(call->args.items[2], s, ctx);
                if (!type_eq(key_type, "STR")) {
                    die_at(((Node *)call->args.items[2])->line, ((Node *)call->args.items[2])->col, "REQ.hostTLS() keyFile must be STR, got %s", key_type);
                }
            }

            char *ctx_type = NULL;
            bool has_ctx = call->args.len == max_args;
            int ctx_index = tls_host ? 3 : 1;
            int handler_index = has_ctx ? (tls_host ? 4 : 2) : (tls_host ? 3 : 1);
            Node *handler = call->args.items[handler_index];
            if (has_ctx) {
                AnalyzeCtx ref_ctx = *ctx;
                ref_ctx.preserve_ref_identity = true;
                ctx_type = analyze_expr(call->args.items[ctx_index], s, &ref_ctx);
                ctx->next_loop_id = ref_ctx.next_loop_id;
                if (!is_ref_type(ctx_type)) {
                    die_at(((Node *)call->args.items[ctx_index])->line, ((Node *)call->args.items[ctx_index])->col, "REQ.%s() context must be REF[T], got %s", call->name, ctx_type);
                }
            }

            if (handler->kind != NK_NAME) {
                die_at(handler->line, handler->col, "REQ.%s() handler must be a top-level function name", call->name);
            }
            FnInfo *fn = fn_find(ctx->fns, handler->name);
            if (!fn) die_at(handler->line, handler->col, "unknown host handler function '%s'", handler->name);
            if (!has_ctx) {
                if (fn->param_count != 1 || (!type_eq(fn->param_types[0], "HttpReq") && !type_eq(fn->param_types[0], "REF[HttpReq]")) || !type_eq(fn->return_type, "HttpRes")) {
                    die_at(handler->line, handler->col, "REQ.%s() handler must be FN(HttpReq) -> HttpRes or FN(REF[HttpReq]) -> HttpRes", call->name);
                }
                handler->declared_type = xstrdup(fn->param_types[0]);
            } else {
                if (fn->param_count != 2 || !type_eq(fn->param_types[0], ctx_type) || (!type_eq(fn->param_types[1], "HttpReq") && !type_eq(fn->param_types[1], "REF[HttpReq]")) || !type_eq(fn->return_type, "HttpRes")) {
                    die_at(handler->line, handler->col, "REQ.%s() stateful handler must be FN(%s, HttpReq) -> HttpRes or FN(%s, REF[HttpReq]) -> HttpRes", call->name, ctx_type, ctx_type);
                }
                handler->declared_type = xstrdup(fn->param_types[1]);
            }
            set_type(handler, "NUL");
            return set_type(call, "NUL");
        }

        if (strcmp(call->name, "ws") == 0 || strcmp(call->name, "wsTLS") == 0) {
            bool tls_ws = strcmp(call->name, "wsTLS") == 0;
            int expected_args = tls_ws ? 4 : 2;
            if (call->args.len != expected_args) {
                if (tls_ws) die_at(call->line, call->col, "REQ.wsTLS() expects INT port, STR certFile, STR keyFile, and FN(STR) -> STR handler");
                die_at(call->line, call->col, "REQ.ws() expects INT port and FN(STR) -> STR handler");
            }
            char *port_type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(port_type, "INT")) {
                die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "REQ.%s() port must be INT, got %s", call->name, port_type);
            }
            if (tls_ws) {
                char *cert_type = analyze_expr(call->args.items[1], s, ctx);
                if (!type_eq(cert_type, "STR")) {
                    die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "REQ.wsTLS() certFile must be STR, got %s", cert_type);
                }
                char *key_type = analyze_expr(call->args.items[2], s, ctx);
                if (!type_eq(key_type, "STR")) {
                    die_at(((Node *)call->args.items[2])->line, ((Node *)call->args.items[2])->col, "REQ.wsTLS() keyFile must be STR, got %s", key_type);
                }
            }
            Node *handler = call->args.items[tls_ws ? 3 : 1];
            if (handler->kind != NK_NAME) {
                die_at(handler->line, handler->col, "REQ.%s() handler must be a top-level function name", call->name);
            }
            FnInfo *fn = fn_find(ctx->fns, handler->name);
            if (!fn) die_at(handler->line, handler->col, "unknown WebSocket handler function '%s'", handler->name);
            if (fn->param_count != 1 || !type_eq(fn->param_types[0], "STR") || !type_eq(fn->return_type, "STR")) {
                die_at(handler->line, handler->col, "REQ.%s() handler must be FN(STR) -> STR", call->name);
            }
            set_type(handler, "NUL");
            return set_type(call, "NUL");
        }

        bool no_body_method = strcmp(call->name, "get") == 0 || strcmp(call->name, "delete") == 0;
        bool body_method = strcmp(call->name, "post") == 0 || strcmp(call->name, "put") == 0;
        if (!no_body_method && !body_method) {
            die_at(call->line, call->col, "REQ.%s() is not implemented yet; use REQ.get/delete(url), REQ.post/put(url, body), REQ.host(...), REQ.hostTLS(...), REQ.ws(...), or REQ.wsTLS(...)", call->name);
        }
        int expected_args = body_method ? 2 : 1;
        if (call->args.len != expected_args) {
            if (body_method) die_at(call->line, call->col, "REQ.%s() expects STR url and STR body", call->name);
            die_at(call->line, call->col, "REQ.%s() expects one STR url", call->name);
        }
        char *url_type = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(url_type, "STR")) {
            die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "REQ.%s() url must be STR, got %s", call->name, url_type);
        }
        if (body_method) {
            char *body_type = analyze_expr(call->args.items[1], s, ctx);
            if (!type_eq(body_type, "STR")) {
                die_at(((Node *)call->args.items[1])->line, ((Node *)call->args.items[1])->col, "REQ.%s() body must be STR, got %s", call->name, body_type);
            }
        }
        return set_type(call, task_type("HttpRes"));
    }

    if (call->object && call->object->kind == NK_NAME) {
        char *qualified = strf("%s_%s", call->object->name, call->name);
        char *display_name = strf("%s.%s", call->object->name, call->name);
        char *constructor_type = analyze_struct_constructor_call(call, s, ctx, qualified, display_name);
        if (constructor_type) {
            set_type(call->object, "NUL");
            return constructor_type;
        }
        if (strcmp(call->object->name, "WEB") == 0 && strcmp(call->name, "json") == 0 && call->args.len == 1) {
            vec_push(&call->args, make_int_node(200, call));
        }
        FnInfo *fn = fn_find(ctx->fns, qualified);
        if (fn) {
            if (call->args.len != fn->param_count) {
                die_at(call->line, call->col, "function '%s.%s' expects %d argument(s), got %d", call->object->name, call->name, fn->param_count, call->args.len);
            }

            for (int i = 0; i < call->args.len; i++) {
                Node *arg = call->args.items[i];
                char *type = analyze_expr_expected(arg, s, ctx, fn->param_types[i]);
                if (!type_eq(type, fn->param_types[i])) {
                    die_at(arg->line, arg->col, "argument %d to '%s.%s' must be %s, got %s", i + 1, call->object->name, call->name, fn->param_types[i], type);
                }
            }

            if (fn->state != 2) analyze_fn(fn, ctx->fns, ctx->types, ctx->global_scope);
            if (fn->returns_owned_ref && is_ref_type(fn->return_type)) call->ref_result_owned = true;
            call->c_expr = qualified;
            set_type(call->object, "NUL");
            return set_type(call, fn->return_type);
        }
    }

    char *object_type = analyze_chain_receiver(call->object, s, ctx);

    Node *fn_field = struct_field(ctx->types, object_type, call->name);
    if (fn_field && is_fn_type(fn_field->declared_type)) {
        if (call->args.len != 1) {
            die_at(call->line, call->col, "function field '%s' expects one argument, got %d", call->name, call->args.len);
        }
        char *input = fn_input_type(fn_field->declared_type);
        char *output = fn_output_type(fn_field->declared_type);
        Node *arg = call->args.items[0];
        char *arg_type = analyze_expr_expected(arg, s, ctx, input);
        if (!type_eq(arg_type, input)) {
            die_at(arg->line, arg->col, "argument to function field '%s' must be %s, got %s", call->name, input, arg_type);
        }
        return set_type(call, output);
    }

    if (strcmp(call->name, "len") == 0) {
        if (call->args.len != 0) die_at(call->line, call->col, ".len() expects no arguments");
        if (!is_array_type(object_type) && !type_eq(object_type, "STR")) {
            die_at(call->line, call->col, ".len() expects ARR or STR receiver, got %s", object_type);
        }
        return set_type(call, "INT");
    }

    if (is_shape_type(object_type)) {
        if (strcmp(call->name, "fill") != 0) {
            die_at(call->line, call->col, "shape literal only supports .fill(value)");
        }
        if (call->body) die_at(call->line, call->col, ".fill() does not take a method body");
        if (call->args.len != 1) die_at(call->line, call->col, ".fill(value) expects one value");
        char *value_type = analyze_expr(call->args.items[0], s, ctx);
        return set_type(call, nested_array_type(value_type, call->object->elements.len));
    }

    if (strcmp(call->name, "ARR") == 0) {
        if (type_eq(object_type, "STR")) {
            if (call->args.len != 0 && call->args.len != 1) die_at(call->line, call->col, ".ARR() expects no arguments or one STR delimiter");
            if (call->args.len == 1) {
                char *delimiter_type = analyze_expr(call->args.items[0], s, ctx);
                if (!type_eq(delimiter_type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, ".ARR(delimiter) expects STR delimiter, got %s", delimiter_type);
            }
            return set_type(call, "ARR[STR]");
        }

        if (is_array_type(object_type)) {
            if (call->args.len != 0) die_at(call->line, call->col, ".ARR() on ARR expects no arguments");
            return set_type(call, object_type);
        }

        if (is_map_type(object_type) || is_struct_type(object_type) || is_open_data_type(object_type) || type_eq(object_type, "OBJ")) {
            if (call->args.len != 0) die_at(call->line, call->col, ".ARR() wrapping a value expects no arguments");
            return set_type(call, array_type(object_type));
        }

        if (call->args.len == 1) {
            char *delimiter_type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(delimiter_type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, ".ARR(delimiter) expects STR delimiter, got %s", delimiter_type);
        }
        die_at(call->line, call->col, ".ARR() expects STR, ARR, MAP, or DYN receiver, got %s", object_type);
    }

    if (strcmp(call->name, "STR") == 0) {
        if (call->args.len != 0 && call->args.len != 1) die_at(call->line, call->col, ".STR() expects no arguments or one STR delimiter");
        if (!is_str_array_type(object_type)) die_at(call->line, call->col, ".STR() expects ARR[STR] receiver, got %s", object_type);
        if (call->args.len == 1) {
            char *delimiter_type = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(delimiter_type, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, ".STR(delimiter) expects STR delimiter, got %s", delimiter_type);
        }
        return set_type(call, "STR");
    }

    if ((is_map_type(object_type) || is_struct_type(object_type) || is_open_data_type(object_type)) &&
        strcmp(call->name, "upd") == 0) {
        if (call->body) die_at(call->line, call->col, "MAP.upd() does not take a method body");
        if (call->args.len != 1) die_at(call->line, call->col, "MAP.upd() expects one MAP or field overlay");
        char *result_type = analyze_collection_plus_right(call, object_type, call->object, call->args.items[0], s, ctx);
        return set_type(call, result_type);
    }

    if (is_array_type(object_type)) {
        char *element_type = array_elem_type(object_type);

        if (strcmp(call->name, "add") == 0) {
            (void)element_type;
            return analyze_array_add_result(call, object_type, s, ctx);
        }

        if (is_collection_method_name(call->name)) {
            Scope *method_scope = collection_method_scope(call, s, element_type, NULL, ctx);

            if (strcmp(call->name, "upd") == 0) {
                char *item_type = NULL;
                if (call->body) {
                    if (call->args.len != 0) die_at(call->line, call->col, ".upd() cannot mix expression arguments and method body");
                    item_type = analyze_method_body(call, method_scope, ctx);
                } else {
                    if (call->args.len != 1) die_at(call->line, call->col, ".upd() expects one expression or a method body");
                    item_type = analyze_expr(call->args.items[0], method_scope, ctx);
                    if (type_eq(item_type, "OPEN") && ((Node *)call->args.items[0])->kind == NK_RECORD_LITERAL) {
                        item_type = synthesize_record_type(ctx->types, call->args.items[0], "Row");
                        set_type(call->args.items[0], item_type);
                    }
                }
                return set_type(call, array_type(item_type));
            }

            if (strcmp(call->name, "filt") == 0) {
                char *predicate_type = NULL;
                if (call->body) {
                    if (call->args.len != 0) die_at(call->line, call->col, ".filt() cannot mix expression arguments and method body");
                    predicate_type = analyze_method_body(call, method_scope, ctx);
                } else {
                    if (call->args.len != 1) die_at(call->line, call->col, ".filt() expects one BOL expression or a method body");
                    predicate_type = analyze_expr(call->args.items[0], method_scope, ctx);
                }
                if (!type_eq(predicate_type, "BOL")) die_at(call->line, call->col, ".filt() predicate must be BOL, got %s", predicate_type);
                return set_type(call, object_type);
            }

            if (strcmp(call->name, "any") == 0) {
                char *predicate_type = NULL;
                if (call->body) {
                    if (call->args.len != 0) die_at(call->line, call->col, ".any() cannot mix expression arguments and method body");
                    predicate_type = analyze_method_body(call, method_scope, ctx);
                } else {
                    if (call->args.len != 1) die_at(call->line, call->col, ".any() expects one BOL expression or a method body");
                    predicate_type = analyze_expr(call->args.items[0], method_scope, ctx);
                }
                if (!type_eq(predicate_type, "BOL")) die_at(call->line, call->col, ".any() predicate must be BOL, got %s", predicate_type);
                return set_type(call, "BOL");
            }

            if (strcmp(call->name, "tap") == 0) {
                if (!ctx->allow_terminal_tap) {
                    die_at(call->line, call->col, ".tap(name)(...) must be followed by another collection method");
                }
                if (call->args.len != 1 || ((Node *)call->args.items[0])->kind != NK_NAME) {
                    die_at(call->line, call->col, ".tap(name)(...) expects one collection alias name");
                }
                if (!call->body) die_at(call->line, call->col, ".tap(name) expects a following statement block");
                Node *alias_arg = call->args.items[0];
                int id = ctx->next_loop_id++;
                call->index_name = strf("tap_%d", id);
                call->alias = xstrdup(alias_arg->name);
                call->result_alias = infer_collection_alias(call->object);

                Scope *tap_scope = scope_new(s);
                scope_define(tap_scope, call->alias, object_type, strf("source_%s", call->index_name), call);
                AnalyzeCtx body_ctx = *ctx;
                body_ctx.enforce_local_style = true;
                body_ctx.allow_terminal_tap = false;
                for (int i = 0; i < call->body->statements.len; i++) {
                    Node *st = call->body->statements.items[i];
                    if (st->kind == NK_RET) die_at(st->line, st->col, ".tap() body cannot return a value");
                    analyze_stmt(st, tap_scope, &body_ctx);
                }
                ctx->next_loop_id = body_ctx.next_loop_id;
                return set_type(call, object_type);
            }

            if (strcmp(call->name, "group") == 0) {
                if (call->args.len != 1) die_at(call->line, call->col, ".group() expects one key expression");
                if (call->body) die_at(call->line, call->col, ".group() does not take a method body");
                char *key_type = analyze_expr(call->args.items[0], method_scope, ctx);
                if (!type_eq(key_type, "STR") && !type_eq(key_type, "INT") && !type_eq(key_type, "BOL")) {
                    die_at(call->line, call->col, ".group() currently supports STR, INT, or BOL keys, got %s", key_type);
                }
                char *group_type = synthesize_group_type(ctx->types, call, key_type, element_type);
                char *result_alias = infer_group_result_alias(call->args.items[0]);
                call->result_alias = result_alias ? result_alias : xstrdup("group");
                call->declared_type = xstrdup(key_type);
                return set_type(call, array_type(group_type));
            }

            if (strcmp(call->name, "sort") == 0) {
                if (call->args.len != 1) die_at(call->line, call->col, ".sort() expects one key expression and optional ASC or DESC");
                if (call->body) die_at(call->line, call->col, ".sort() does not take a method body");
                char *key_type = analyze_expr(call->args.items[0], method_scope, ctx);
                if (!type_eq(key_type, "INT") && !type_eq(key_type, "FLT") && !type_eq(key_type, "STR")) {
                    die_at(call->line, call->col, ".sort() currently supports INT, FLT, or STR keys, got %s", key_type);
                }
                call->declared_type = xstrdup(key_type);
                if (!call->sort_dir) call->sort_dir = xstrdup("ASC");
                return set_type(call, object_type);
            }

            if (strcmp(call->name, "agg") == 0) {
                char *value_type = analyze_agg_value(call, method_scope, ctx, element_type);
                if (!type_eq(value_type, "INT") && !type_eq(value_type, "FLT") && !type_eq(value_type, "STR")) {
                    die_at(call->line, call->col, ".agg() currently supports INT, FLT, or STR accumulation, got %s", value_type);
                }
                return set_type(call, value_type);
            }
        }
    }

    if (type_eq(object_type, "STR") && is_collection_method_name(call->name)) {
        Scope *method_scope = collection_method_scope(call, s, "STR", NULL, ctx);
        Symbol *elem_sym = scope_resolve(method_scope, "e", call);
        elem_sym->c_expr = xstrdup(call->element_ptr);
        if (call->alias) {
            Symbol *alias_sym = scope_resolve(method_scope, call->alias, call);
            alias_sym->c_expr = xstrdup(call->element_ptr);
        }

        if (strcmp(call->name, "upd") == 0) {
            char *item_type = NULL;
            if (call->body) {
                if (call->args.len != 0) die_at(call->line, call->col, ".upd() cannot mix expression arguments and method body");
                item_type = analyze_method_body(call, method_scope, ctx);
            } else {
                if (call->args.len != 1) die_at(call->line, call->col, ".upd() expects one STR expression or a method body");
                item_type = analyze_expr(call->args.items[0], method_scope, ctx);
            }
            if (!type_eq(item_type, "STR")) die_at(call->line, call->col, "STR.upd() must produce STR, got %s", item_type);
            return set_type(call, "STR");
        }

        if (strcmp(call->name, "filt") == 0) {
            char *predicate_type = NULL;
            if (call->body) {
                if (call->args.len != 0) die_at(call->line, call->col, ".filt() cannot mix expression arguments and method body");
                predicate_type = analyze_method_body(call, method_scope, ctx);
            } else {
                if (call->args.len != 1) die_at(call->line, call->col, ".filt() expects one BOL expression or a method body");
                predicate_type = analyze_expr(call->args.items[0], method_scope, ctx);
            }
            if (!type_eq(predicate_type, "BOL")) die_at(call->line, call->col, "STR.filt() predicate must be BOL, got %s", predicate_type);
            return set_type(call, "STR");
        }

        if (strcmp(call->name, "any") == 0) {
            char *predicate_type = NULL;
            if (call->body) {
                if (call->args.len != 0) die_at(call->line, call->col, ".any() cannot mix expression arguments and method body");
                predicate_type = analyze_method_body(call, method_scope, ctx);
            } else {
                if (call->args.len != 1) die_at(call->line, call->col, ".any() expects one BOL expression or a method body");
                predicate_type = analyze_expr(call->args.items[0], method_scope, ctx);
            }
            if (!type_eq(predicate_type, "BOL")) die_at(call->line, call->col, "STR.any() predicate must be BOL, got %s", predicate_type);
            return set_type(call, "BOL");
        }

        if (strcmp(call->name, "agg") == 0) {
            char *value_type = analyze_agg_value(call, method_scope, ctx, "STR");
            if (!type_eq(value_type, "INT") && !type_eq(value_type, "FLT") && !type_eq(value_type, "STR")) {
                die_at(call->line, call->col, "STR.agg() currently supports INT, FLT, or STR accumulation, got %s", value_type);
            }
            return set_type(call, value_type);
        }
    }

    if (is_map_type(object_type)) {
        char *key_type = map_key_type(object_type);
        char *value_type = map_value_type(object_type);

        if (strcmp(call->name, "add") == 0) {
            die_at(call->line, call->col, ".add() is not used for MAP; use map = map + otherMap");
        }

        if (strcmp(call->name, "has") == 0) {
            if (call->args.len != 1) die_at(call->line, call->col, ".has() on MAP expects key");
            char *actual_key = analyze_expr(call->args.items[0], s, ctx);
            if (!type_eq(actual_key, key_type)) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "map key must be %s, got %s", key_type, actual_key);
            return set_type(call, "BOL");
        }

        if (strcmp(call->name, "agg") == 0) {
            Scope *method_scope = map_method_scope(call, s, key_type, value_type, ctx);
            char *agg_type = analyze_agg_value(call, method_scope, ctx, value_type);
            if (!type_eq(agg_type, "INT") && !type_eq(agg_type, "FLT") && !type_eq(agg_type, "STR")) {
                die_at(call->line, call->col, "MAP.agg() currently supports INT, FLT, or STR accumulation, got %s", agg_type);
            }
            return set_type(call, agg_type);
        }
    }

    if ((is_struct_type(object_type) || is_open_data_type(object_type) || type_eq(object_type, "OBJ")) && strcmp(call->name, "has") == 0) {
        if (is_struct_type(object_type)) mark_struct_open(ctx->types, object_type);
        if (call->args.len != 1) die_at(call->line, call->col, ".has() on MAP expects key");
        char *actual_key = analyze_expr(call->args.items[0], s, ctx);
        if (!type_eq(actual_key, "STR")) die_at(((Node *)call->args.items[0])->line, ((Node *)call->args.items[0])->col, "open MAP key must be STR, got %s", actual_key);
        return set_type(call, "BOL");
    }

    die_at(call->line, call->col, "method '.%s()' is not implemented in this pass", call->name);
    return NULL;
}
