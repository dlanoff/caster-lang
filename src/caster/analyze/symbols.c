// Analyzer scopes, symbols, source-key tracking, and function registry.

// ----------------------------- Scopes -----------------------------

typedef struct {
    char *key;
    char *type;
} SourceKeyType;

typedef enum {
    OWNERSHIP_UNKNOWN,
    OWNERSHIP_BORROWED,
    OWNERSHIP_OWNED,
    OWNERSHIP_FREED
} OwnershipState;

typedef struct {
    char *name;
    char *type;
    char *c_expr;
    char *ctx_index;
    char *ctx_element;
    char *ctx_element_type;
    int ctx_min_depth;
    char *ref_source_name;
    char *ref_source_type;
    OwnershipState ownership;
    bool is_param;
    bool is_global;
    PtrVec source_literal_keys;
    PtrVec source_literal_key_types;
    Node *node;
} Symbol;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    Symbol *items;
    int len;
    int cap;
};

// Creates a child scope, which bounds local names and ownership because Caster functions, loops,
// and method bodies each introduce local context.
static Scope *scope_new(Scope *parent) {
    Scope *s = xmalloc(sizeof(Scope));
    memset(s, 0, sizeof(Scope));
    s->parent = parent;
    return s;
}

// Looks up a name only in the current scope, which catches duplicate declarations because
// shadowing rules differ from resolution rules.
static Symbol *scope_lookup_current(Scope *s, const char *name) {
    for (int i = 0; i < s->len; i++) {
        if (strcmp(s->items[i].name, name) == 0) return &s->items[i];
    }
    return NULL;
}

// Searches parent scopes for a symbol, which supports nested helpers and loop bodies because
// readable code depends on lexical lookup.
static Symbol *scope_try_resolve(Scope *s, const char *name) {
    for (Scope *cur = s; cur; cur = cur->parent) {
        Symbol *sym = scope_lookup_current(cur, name);
        if (sym) return sym;
    }
    return NULL;
}

// Resolves a required symbol, which reports unknown names at the source site because later
// analyzer code assumes successful lookup.
static Symbol *scope_resolve(Scope *s, const char *name, Node *node) {
    Symbol *sym = scope_try_resolve(s, name);
    if (sym) return sym;
    die_at(node->line, node->col, "unknown name '%s'", name);
    return NULL;
}

// Defines a symbol in the current scope, which records type, C expression, and ownership because
// one symbol table feeds both analysis and emission metadata.
static Symbol *scope_define(Scope *s, const char *name, const char *type, const char *c_expr, Node *node) {
    if (scope_lookup_current(s, name)) die_at(node->line, node->col, "redeclaration of '%s'", name);
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = xrealloc(s->items, sizeof(Symbol) * (size_t)s->cap);
    }
    Symbol *sym = &s->items[s->len++];
    memset(sym, 0, sizeof(Symbol));
    sym->name = xstrdup(name);
    sym->type = xstrdup(type);
    sym->c_expr = c_expr ? xstrdup(c_expr) : NULL;
    sym->ownership = OWNERSHIP_UNKNOWN;
    sym->node = node;
    return sym;
}

// Defines a loop frame symbol, which keeps grid.i/grid.e access available because nested loops
// shadow plain i/e.
static void scope_define_ctx(Scope *s, const char *name, const char *type, const char *index_expr, const char *element_expr, Node *node) {
    Symbol *sym = scope_define(s, name, type, NULL, node);
    sym->ctx_index = index_expr ? xstrdup(index_expr) : NULL;
    sym->ctx_element = element_expr ? xstrdup(element_expr) : NULL;
    sym->ctx_element_type = element_expr && is_loop_ctx_type(type) ? loop_ctx_element_type(type) : NULL;
}

// Recognizes compiler-provided loop names, which prevents users from treating e/i/val as durable
// sources because they are contextual bindings.
static bool is_implicit_loop_source_name(const char *name) {
    return name && strcmp(name, "e") != 0 && strcmp(name, "i") != 0 && strcmp(name, "val") != 0;
}

static void scope_define_loop_source_ctx(
    Scope *s,
    Scope *parent,
    Node *source,
    const char *index_expr,
    const char *element_expr,
    const char *element_type,
    int min_depth) {
    if (!source || source->kind != NK_NAME || !is_implicit_loop_source_name(source->name)) return;
    if (scope_lookup_current(s, source->name)) return;

    Symbol *base = scope_try_resolve(parent, source->name);
    if (!base) return;

    Symbol *sym = scope_define(s, source->name, base->type, base->c_expr, source);
    sym->ownership = base->ownership;
    sym->is_param = base->is_param;
    sym->is_global = base->is_global;
    sym->ctx_index = index_expr ? xstrdup(index_expr) : NULL;
    sym->ctx_element = element_expr ? xstrdup(element_expr) : NULL;
    sym->ctx_element_type = element_type ? xstrdup(element_type) : NULL;
    sym->ctx_min_depth = min_depth;
}

// Checks a small string vector, which avoids heavier containers because analyzer key sets are tiny
// and local.
static bool ptrvec_has_string(PtrVec *vec, const char *value) {
    for (int i = 0; i < vec->len; i++) {
        if (strcmp((char *)vec->items[i], value) == 0) return true;
    }
    return false;
}

// Adds a string once to a vector, which preserves key-set facts without duplicates because dynamic
// MAP refinements are set-like.
static void ptrvec_push_unique_string(PtrVec *vec, const char *value) {
    if (!value || ptrvec_has_string(vec, value)) return;
    vec_push(vec, xstrdup(value));
}

// Finds a recorded dynamic key type, which keeps open MAP dot access precise because literal keys
// can be known even on runtime maps.
static SourceKeyType *source_key_type_find(PtrVec *vec, const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < vec->len; i++) {
        SourceKeyType *entry = vec->items[i];
        if (strcmp(entry->key, key) == 0) return entry;
    }
    return NULL;
}

// Records a dynamic key type, which lets later reads infer that key because overlays and literals
// can prove fields on open data.
static void source_key_type_set(PtrVec *vec, const char *key, const char *type) {
    if (!key || !type) return;
    SourceKeyType *entry = source_key_type_find(vec, key);
    if (entry) {
        entry->type = xstrdup(type);
        return;
    }
    entry = xmalloc(sizeof(SourceKeyType));
    entry->key = xstrdup(key);
    entry->type = xstrdup(type);
    vec_push(vec, entry);
}

// Removes a dynamic key fact, which keeps -= and mutation honest because deleted keys should no
// longer type-check as present.
static void source_key_type_remove(PtrVec *vec, const char *key) {
    if (!key) return;
    for (int i = 0; i < vec->len; i++) {
        SourceKeyType *entry = vec->items[i];
        if (strcmp(entry->key, key) == 0) {
            for (int j = i + 1; j < vec->len; j++) vec->items[j - 1] = vec->items[j];
            vec->len--;
            return;
        }
    }
}

// Drops a proven literal key from a symbol, which reflects dynamic key removal because source-key
// facts track the current value.
static void symbol_remove_source_literal_key(Symbol *sym, const char *key) {
    if (!sym || !key) return;
    for (int i = 0; i < sym->source_literal_keys.len; i++) {
        if (strcmp((char *)sym->source_literal_keys.items[i], key) == 0) {
            for (int j = i + 1; j < sym->source_literal_keys.len; j++) {
                sym->source_literal_keys.items[j - 1] = sym->source_literal_keys.items[j];
            }
            sym->source_literal_keys.len--;
            source_key_type_remove(&sym->source_literal_key_types, key);
            return;
        }
    }
    source_key_type_remove(&sym->source_literal_key_types, key);
}

// Collects literal keys from an expression, which refines open MAP locals because object literals
// and overlays reveal stable fields.
static void collect_source_literal_keys(Node *expr, Scope *s, TypeTable *types, PtrVec *keys) {
    if (!expr) return;

    if (expr->kind == NK_MAP_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->target && entry->target->kind == NK_STR) {
                ptrvec_push_unique_string(keys, entry->target->text);
            }
        }
        return;
    }

    if (expr->kind == NK_RECORD_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->name) ptrvec_push_unique_string(keys, entry->name);
        }
        return;
    }

    if (expr->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (sym) {
            for (int i = 0; i < sym->source_literal_keys.len; i++) {
                ptrvec_push_unique_string(keys, sym->source_literal_keys.items[i]);
            }
            return;
        }

        TypeInfo *info = type_find(types, expr->name);
        if (info && info->kind == TYPEINFO_ALIAS && info->node->value && is_map_type(info->target)) {
            collect_source_literal_keys(info->node->value, s, types, keys);
        }
        return;
    }

    if (expr->kind == NK_BINARY && expr->op && strcmp(expr->op, "+") == 0) {
        collect_source_literal_keys(expr->left, s, types, keys);
        collect_source_literal_keys(expr->right, s, types, keys);
    }
}

// Collects literal key types from an expression, which supports typed dot reads on open data
// because value types can be known per key.
static void collect_source_literal_key_types(Node *expr, Scope *s, TypeTable *types, PtrVec *key_types) {
    if (!expr) return;

    if (expr->kind == NK_MAP_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->target && entry->target->kind == NK_STR && entry->value && entry->value->checked_type) {
                source_key_type_set(key_types, entry->target->text, entry->value->checked_type);
            }
        }
        return;
    }

    if (expr->kind == NK_RECORD_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->name && entry->value && entry->value->checked_type) {
                source_key_type_set(key_types, entry->name, entry->value->checked_type);
            }
        }
        return;
    }

    if (expr->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (sym) {
            for (int i = 0; i < sym->source_literal_key_types.len; i++) {
                SourceKeyType *entry = sym->source_literal_key_types.items[i];
                source_key_type_set(key_types, entry->key, entry->type);
            }
            return;
        }

        TypeInfo *info = type_find(types, expr->name);
        if (info && info->kind == TYPEINFO_ALIAS && info->node->value && is_map_type(info->target)) {
            collect_source_literal_key_types(info->node->value, s, types, key_types);
            return;
        }

        if (info && info->kind == TYPEINFO_STRUCT) {
            for (int i = 0; i < info->node->fields.len; i++) {
                Node *field = info->node->fields.items[i];
                source_key_type_set(key_types, field->name, field->declared_type);
            }
        }
        return;
    }

    if (expr->kind == NK_BINARY && expr->op && strcmp(expr->op, "+") == 0) {
        collect_source_literal_key_types(expr->left, s, types, key_types);
        collect_source_literal_key_types(expr->right, s, types, key_types);
    }
}

// Replaces a symbol key set from an expression, which keeps assignment refinements exact because a
// new value discards old dynamic fields.
static void symbol_set_source_literal_keys_from_expr(Symbol *sym, Node *expr, Scope *s, TypeTable *types) {
    if (!sym) return;
    PtrVec keys = {0};
    collect_source_literal_keys(expr, s, types, &keys);
    sym->source_literal_keys.len = 0;
    for (int i = 0; i < keys.len; i++) {
        ptrvec_push_unique_string(&sym->source_literal_keys, keys.items[i]);
    }

    PtrVec key_types = {0};
    collect_source_literal_key_types(expr, s, types, &key_types);
    sym->source_literal_key_types.len = 0;
    for (int i = 0; i < key_types.len; i++) {
        SourceKeyType *entry = key_types.items[i];
        source_key_type_set(&sym->source_literal_key_types, entry->key, entry->type);
    }
}

// Adds proven keys from an overlay expression, which preserves existing open MAP facts because
// joins extend rather than replace shape.
static void symbol_add_source_literal_keys_from_expr(Symbol *sym, Node *expr, Scope *s, TypeTable *types) {
    if (!sym) return;
    collect_source_literal_keys(expr, s, types, &sym->source_literal_keys);
    collect_source_literal_key_types(expr, s, types, &sym->source_literal_key_types);
}

// Checks whether an expression proves a key, which lets dot access on open data succeed only when the analyzer has evidence.
static bool expr_has_source_literal_key(Node *expr, Scope *s, TypeTable *types, const char *key) {
    PtrVec keys = {0};
    collect_source_literal_keys(expr, s, types, &keys);
    return ptrvec_has_string(&keys, key);
}

// Gets the proven type for a literal key, which keeps open MAP reads typed because DYN-like data
// can still carry known fields.
static char *expr_source_literal_key_type(Node *expr, Scope *s, TypeTable *types, const char *key) {
    PtrVec key_types = {0};
    collect_source_literal_key_types(expr, s, types, &key_types);
    SourceKeyType *entry = source_key_type_find(&key_types, key);
    return entry ? entry->type : NULL;
}

// Refines a symbol after inference, which updates the declaration node too because the emitter
// reads types from the AST.
static void refine_symbol_type(Symbol *sym, const char *type) {
    sym->type = xstrdup(type);
    if (sym->node && sym->node->inferred_decl) {
        sym->node->declared_type = xstrdup(type);
        if (sym->node->value && is_unknown_array_type(sym->node->value->checked_type)) {
            set_type(sym->node->value, type);
        }
    }
}

// ----------------------------- Functions -----------------------------

typedef struct {
    char *name;
    Node *node;
    char **param_types;
    int param_count;
    char *declared_return_type;
    char *return_type;
    bool returns_owned_ref;
    int state;
} FnInfo;

typedef struct {
    FnInfo *items;
    int len;
    int cap;
} FnTable;

// Adds function metadata, which supports forward references because functions can be used before
// their bodies are analyzed.
static void fntable_push(FnTable *t, FnInfo f) {
    if (t->len == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->items = xrealloc(t->items, sizeof(FnInfo) * (size_t)t->cap);
    }
    t->items[t->len++] = f;
}

// Finds function metadata by compiled name, which resolves nested helper calls because user names
// may lower to unique C symbols.
static FnInfo *fn_find(FnTable *t, const char *name) {
    if (!t || !name) return NULL;
    for (int i = 0; i < t->len; i++) {
        if (strcmp(t->items[i].name, name) == 0) return &t->items[i];
    }
    return NULL;
}

// Creates stable C names for nested helpers, which avoids collisions because different outer
// functions may define helpers with the same source name.
static char *nested_function_c_name(const char *owner, Node *fn) {
    if (!owner) owner = "script";
    return strf("flx_nested_%s_%s_%d_%d",
        analyzer_lower_mangle(owner),
        analyzer_lower_mangle(fn->name),
        fn->line,
        fn->col);
}

// Registers a function signature before body analysis, which allows mutual ordering and calls
// above definitions because signatures are the typed contract.
static FnInfo *register_function_info(FnTable *table, TypeTable *types, Node *fn, const char *table_name) {
    if (fn_find(table, table_name)) die_at(fn->line, fn->col, "duplicate function '%s'", fn->name);

    FnInfo info = {0};
    info.name = xstrdup(table_name);
    info.node = fn;
    info.param_count = fn->params.len;
    info.param_types = xmalloc(sizeof(char *) * (size_t)info.param_count);

    for (int j = 0; j < info.param_count; j++) {
        Node *param = fn->params.items[j];
        param->declared_type = resolve_type(types, param->declared_type, param);
        info.param_types[j] = param->declared_type;
    }

    if (!fn->declared_type) {
        die_at(fn->line, fn->col, "function '%s' must declare an explicit return type; use -> NUL for no value", fn->name);
    }

    fn->declared_type = resolve_type(types, fn->declared_type, fn);
    info.declared_return_type = fn->declared_type;
    info.return_type = xstrdup(fn->declared_type);

    fntable_push(table, info);
    return &table->items[table->len - 1];
}

// Registers nested helpers recursively, which preserves lexical organization because nested
// functions still need global C-level declarations.
static void register_nested_functions_in_block(FnTable *table, TypeTable *types, Node *b, const char *owner) {
    if (!b) return;
    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind == NK_FN) {
            if (strcmp(st->name, "main") == 0) {
                die_at(st->line, st->col, "nested function cannot be named main");
            }
            if (!st->c_expr) st->c_expr = nested_function_c_name(owner, st);
            register_function_info(table, types, st, st->c_expr);
            register_nested_functions_in_block(table, types, st->body, st->name);
            continue;
        }
        if (st->kind == NK_LOOP) {
            register_nested_functions_in_block(table, types, st->body, owner);
        } else if (st->kind == NK_IF) {
            register_nested_functions_in_block(table, types, st->then_block, owner);
            for (int j = 0; j < st->elx_branches.len; j++) {
                Node *branch = st->elx_branches.items[j];
                register_nested_functions_in_block(table, types, branch->body, owner);
            }
            register_nested_functions_in_block(table, types, st->else_block, owner);
        }
    }
}

typedef struct {
    FnInfo *fn;
    FnTable *fns;
    TypeTable *types;
    Scope *global_scope;
    char *return_type;
    bool saw_return;
    bool enforce_local_style;
    bool allow_json_decode_boundary;
    bool allow_terminal_tap;
    bool preserve_ref_identity;
    int next_loop_id;
    int loop_depth;
} AnalyzeCtx;
