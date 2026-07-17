// Analyzer type table, type resolution, and built-in MAP values.

// Checks built-in type names, which prevents user declarations from colliding with core syntax
// because primitive and container names have fixed meaning.
static bool is_builtin_type_name(const char *name) {
    return type_eq(name, "INT") || type_eq(name, "FLT") || type_eq(name, "BOL") || type_eq(name, "STR") || type_eq(name, "NUL") || type_eq(name, "ARR") || type_eq(name, "MAP") || type_eq(name, "REF") || type_eq(name, "TSK");
}

// Looks up declared type metadata, which keeps aliases and MAP definitions centralized because
// every phase needs the same resolved type table.
static TypeInfo *type_find(TypeTable *types, const char *name) {
    if (!types) return NULL;
    for (int i = 0; i < types->len; i++) {
        if (strcmp(types->items[i].name, name) == 0) return &types->items[i];
    }
    return NULL;
}

// Adds a type declaration to the table, which catches duplicate and reserved names early because
// later resolution assumes names are unique.
static void type_push(TypeTable *types, TypeInfo info) {
    if (type_find(types, info.name)) {
        die_at(info.node->line, info.node->col, "duplicate TYPE declaration '%s'", info.name);
    }
    if (is_builtin_type_name(info.name)) {
        die_at(info.node->line, info.node->col, "TYPE name '%s' conflicts with a built-in type", info.name);
    }
    if (types->len == types->cap) {
        types->cap = types->cap ? types->cap * 2 : 8;
        types->items = xrealloc(types->items, sizeof(TypeInfo) * (size_t)types->cap);
    }
    types->items[types->len++] = info;
}

static char *resolve_type(TypeTable *types, const char *raw, Node *at);

// Splits a MAP type into key and value text, which supports nested MAP parsing because commas only
// count at the outer bracket depth.
static void split_map_type(const char *type, char **key_out, char **value_out) {
    if (!is_map_type(type)) {
        *key_out = NULL;
        *value_out = NULL;
        return;
    }

    const char *start = type + 4;
    const char *end = type + strlen(type) - 1;
    int depth = 0;
    const char *comma = NULL;

    for (const char *p = start; p < end; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if (*p == ',' && depth == 0) {
            comma = p;
            break;
        }
    }

    if (!comma) {
        *key_out = NULL;
        *value_out = NULL;
        return;
    }

    *key_out = xstrndup(start, (size_t)(comma - start));
    *value_out = xstrndup(comma + 1, (size_t)(end - comma - 1));
}

// Extracts the key type from MAP syntax, which keeps map analysis concise because key checks
// happen in many paths.
static char *map_key_type(const char *type) {
    char *key = NULL;
    char *value = NULL;
    split_map_type(type, &key, &value);
    return key;
}

// Extracts the value type from MAP syntax, which keeps map analysis concise because iteration,
// joins, and decoding all need the payload type.
static char *map_value_type(const char *type) {
    char *key = NULL;
    char *value = NULL;
    split_map_type(type, &key, &value);
    return value;
}

// Checks for loop context pseudo-types, which distinguishes frame metadata from user values
// because qualified grid.i/grid.e are compiler-created.
static bool is_loop_ctx_type(const char *type) {
    return type && strncmp(type, "CTX[", 4) == 0 && type[strlen(type) - 1] == ']';
}

// Builds a loop context pseudo-type, which gives loop frames a typed symbol because qualified
// access needs element metadata.
static char *loop_ctx_type(const char *element_type) {
    return strf("CTX[%s]", element_type);
}

// Extracts the element type from a loop context, which lets qualified loop access reuse ordinary
// dot analysis because frames carry their source element.
static char *loop_ctx_element_type(const char *type) {
    size_t len = strlen(type);
    return xstrndup(type + 4, len - 5);
}

// Checks unresolved inference markers, which lets later assignments refine them because empty
// literals start without enough information.
static bool is_unknown_type(const char *type) {
    return type_eq(type, "?") || type_eq(type, "ARR[?]");
}

// Checks shape literals, which keeps matrix fill typing separate because [10x10] is a dimension
// descriptor before .fill creates data.
static bool is_shape_type(const char *type) {
    return type_eq(type, "SHAPE");
}

// Checks dynamic open data, which keeps runtime-key objects distinct because they lower
// differently from fixed MAP structs.
static bool is_open_data_type(const char *type) {
    return type_eq(type, "OPEN");
}

// Checks unresolved array literals, which lets empty arrays conform later because [] has no
// element evidence by itself.
static bool is_unknown_array_type(const char *type) {
    return type_eq(type, "ARR[?]");
}

// Mangles type names for generated helper names, which keeps internal symbols valid C because
// Caster types can contain brackets and punctuation.
static char *analyzer_lower_mangle(const char *type) {
    char *out = NULL;
    if (type_eq(type, "INT") || type_eq(type, "FLT") || type_eq(type, "BOL") || type_eq(type, "STR")) {
        out = xstrdup(type);
    } else if (is_array_type(type)) {
        out = strf("ARR_%s", analyzer_lower_mangle(array_elem_type(type)));
    } else if (is_task_type(type)) {
        out = strf("TSK_%s", analyzer_lower_mangle(task_result_type(type)));
    } else if (is_fn_type(type)) {
        out = strf("FN_%s_TO_%s", analyzer_lower_mangle(fn_input_type(type)), analyzer_lower_mangle(fn_output_type(type)));
    } else {
        out = xstrdup(type);
    }
    for (char *p = out; *p; p++) {
        if (isalnum((unsigned char)*p)) *p = (char)tolower((unsigned char)*p);
        else *p = '_';
    }
    return out;
}

// Builds the storage symbol for named MAP values, which keeps top-level named data readable in C
// because values and type names share source spelling.
static char *named_value_storage_name(const char *name) {
    return strf("flx_named_%s", name);
}

// Resolves a type alias through the table, which detects cycles because aliases can refer to other
// aliases before their final target is known.
static char *resolve_alias(TypeTable *types, TypeInfo *info, Node *at) {
    if (info->kind == TYPEINFO_STRUCT) return xstrdup(info->name);
    if (info->target) return xstrdup(info->target);
    if (info->resolving) die_at(at->line, at->col, "cyclic TYPE alias involving '%s'", info->name);

    info->resolving = 1;
    info->target = resolve_type(types, info->target_raw, info->node);
    info->resolving = 0;
    return xstrdup(info->target);
}

// Resolves source type syntax to canonical analyzer types, which normalizes aliases, DYN, REF,
// TSK, MAP, and FN because all later checks compare canonical strings.
static char *resolve_type(TypeTable *types, const char *raw, Node *at) {
    if (type_eq(raw, "INT") || type_eq(raw, "FLT") || type_eq(raw, "BOL") || type_eq(raw, "STR") || type_eq(raw, "NUL")) {
        return xstrdup(raw);
    }
    if (type_eq(raw, "DYN")) return xstrdup("OBJ");

    if (is_array_type(raw)) {
        char *elem = array_elem_type(raw);
        char *resolved_elem = resolve_type(types, elem, at);
        return array_type(resolved_elem);
    }

    if (is_map_type(raw)) {
        char *key = map_key_type(raw);
        char *value = map_value_type(raw);
        if (!key || !value) die_at(at->line, at->col, "invalid MAP type '%s'", raw);
        char *resolved_key = resolve_type(types, key, at);
        char *resolved_value = resolve_type(types, value, at);
        return map_type(resolved_key, resolved_value);
    }

    if (is_ref_type(raw)) {
        char *target = ref_target_type(raw);
        char *resolved_target = resolve_type(types, target, at);
        return ref_type(resolved_target);
    }

    if (is_task_type(raw)) {
        char *target = task_result_type(raw);
        char *resolved_target = resolve_type(types, target, at);
        return task_type(resolved_target);
    }

    if (is_fn_type(raw)) {
        char *input = fn_input_type(raw);
        char *output = fn_output_type(raw);
        if (!input || !output) die_at(at->line, at->col, "invalid FN type '%s'", raw);
        char *resolved_input = resolve_type(types, input, at);
        char *resolved_output = resolve_type(types, output, at);
        return fn_type(resolved_input, resolved_output);
    }

    TypeInfo *info = type_find(types, raw);
    if (info) return resolve_alias(types, info, at);

    die_at(at->line, at->col, "unknown type '%s'", raw);
    return NULL;
}

// Checks whether a type is a closed MAP struct, which selects fixed-field semantics because closed
// values can lower to readable C structs.
static bool is_struct_type(const char *type) {
    TypeInfo *info = type_find(g_types, type);
    return info && info->kind == TYPEINFO_STRUCT;
}

// Checks whether a named MAP has been forced open, which preserves dynamic key behavior because
// stable and runtime-key MAPs share one source concept.
static bool is_open_struct_type(const char *type) {
    TypeInfo *info = type_find(g_types, type);
    return info && info->kind == TYPEINFO_STRUCT && info->open_runtime;
}

// Marks a named MAP as open at runtime, which prevents fixed-field lowering after dynamic key use
// because the backend must honor actual key behavior.
static void mark_struct_open(TypeTable *types, const char *type) {
    TypeInfo *info = type_find(types, type);
    if (info && info->kind == TYPEINFO_STRUCT) info->open_runtime = true;
}

// Finds a fixed MAP field declaration, which lets dot access and overlays validate fields because
// closed MAPs have known layouts.
static Node *struct_field(TypeTable *types, const char *struct_name, const char *field_name) {
    TypeInfo *info = type_find(types, struct_name);
    if (!info || info->kind != TYPEINFO_STRUCT) return NULL;

    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        if (strcmp(field->name, field_name) == 0) return field;
    }
    return NULL;
}

static bool type_has_complete_named_value(TypeTable *types, TypeInfo *info);

// Checks whether a field can initialize itself, which validates INIT and nested named values
// because named MAP defaults must be complete.
static bool field_has_default(TypeTable *types, Node *field) {
    if (field->value) return true;
    TypeInfo *info = field->text ? type_find(types, field->text) : NULL;
    if (info && type_has_complete_named_value(types, info)) return true;
    if (is_array_type(field->declared_type) || is_map_type(field->declared_type)) return true;
    if (is_ref_type(field->declared_type) || is_fn_type(field->declared_type)) return true;
    return false;
}

// Checks named value completeness recursively, which avoids infinite alias walks because nested
// INIT can refer through the type table.
static bool type_has_complete_named_value_depth(TypeTable *types, TypeInfo *info, int depth) {
    if (!info) return false;
    if (depth > types->len) return false;
    if (info->kind == TYPEINFO_ALIAS) return info->node->value != NULL;

    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        if (field->value) continue;

        return false;
    }

    return true;
}

// Checks whether a named MAP value has all defaults, which allows INIT only when construction has a complete source value.
static bool type_has_complete_named_value(TypeTable *types, TypeInfo *info) {
    return type_has_complete_named_value_depth(types, info, 0);
}

static void ensure_builtin_httpreq(TypeTable *types);
static void ensure_builtin_httpres(TypeTable *types);
static void ensure_builtin_filestat(TypeTable *types);
static void ensure_builtin_procresult(TypeTable *types);
static void ensure_builtin_sqldb(TypeTable *types);
static void ensure_builtin_sqlexec(TypeTable *types);

// Registers nested MAP declarations before their parent fields resolve, which supports readable
// nested schemas because inner types need names in the table.
static void push_type_decl_recursive(TypeTable *types, Node *decl) {
    for (int i = 0; i < decl->type_decls.len; i++) {
        push_type_decl_recursive(types, decl->type_decls.items[i]);
    }

    TypeInfo info = {0};
    info.name = decl->name;
    info.node = decl;

    if (decl->kind == NK_TYPE_ALIAS) {
        info.kind = TYPEINFO_ALIAS;
        info.target_raw = decl->declared_type;
    } else {
        info.kind = TYPEINFO_STRUCT;
    }

    type_push(types, info);
}

// Builds and resolves the complete type table, which front-loads type validation because
// expression analysis relies on canonical declarations.
static void build_type_table(Node *prog, TypeTable *types) {
    for (int i = 0; i < prog->type_decls.len; i++) {
        Node *decl = prog->type_decls.items[i];
        push_type_decl_recursive(types, decl);
    }

    ensure_builtin_httpreq(types);
    ensure_builtin_httpres(types);
    ensure_builtin_filestat(types);
    ensure_builtin_procresult(types);
    ensure_builtin_sqldb(types);
    ensure_builtin_sqlexec(types);

    for (int i = 0; i < types->len; i++) {
        TypeInfo *info = &types->items[i];
        if (info->kind == TYPEINFO_ALIAS) {
            info->target = resolve_type(types, info->target_raw, info->node);
            info->node->checked_type = xstrdup(info->target);
        } else {
            info->node->checked_type = xstrdup(info->name);
            for (int j = 0; j < info->node->fields.len; j++) {
                Node *field = info->node->fields.items[j];
                char *resolved = resolve_type(types, field->declared_type, field);
                field->declared_type = resolved;
                set_type(field, resolved);
            }
        }
    }
}

// Creates a synthetic field declaration, which lets native structs reuse normal MAP machinery
// because built-ins should behave like user types.
static Node *builtin_field(const char *type, const char *name, Node *value) {
    Token tok = {TK_NAME, xstrdup((char *)name), 1, 1};
    Node *field = node_new(NK_FIELD, tok);
    field->declared_type = xstrdup(type);
    field->name = xstrdup(name);
    field->text = xstrdup(type);
    field->value = value;
    return field;
}

// Creates a synthetic INT default, which keeps native MAP defaults analyzer-visible because
// built-ins are not parsed from source.
static Node *builtin_int_value(int64_t value) {
    Token tok = {TK_INT_LITERAL, xstrdup("0"), 1, 1};
    Node *n = node_new(NK_INT, tok);
    n->int_value = value;
    return n;
}

// Creates a synthetic BOL default, which keeps native MAP defaults analyzer-visible because
// built-ins are not parsed from source.
static Node *builtin_bol_value(bool value) {
    Token tok = {TK_BOL_LITERAL, xstrdup(value ? "true" : "false"), 1, 1};
    Node *n = node_new(NK_BOL, tok);
    n->bool_value = value;
    return n;
}

// Creates a synthetic STR default, which keeps native MAP defaults analyzer-visible because
// built-ins are not parsed from source.
static Node *builtin_str_value(const char *value) {
    Token tok = {TK_STRING_LITERAL, xstrdup((char *)value), 1, 1};
    Node *n = node_new(NK_STR, tok);
    n->text = xstrdup(value);
    return n;
}

// Creates a synthetic MAP default, which lets native headers/context fields start empty because
// built-ins must be constructible like user maps.
static Node *builtin_map_value(void) {
    Token tok = {TK_LBRACE, xstrdup("{"), 1, 1};
    return node_new(NK_MAP_LITERAL, tok);
}

// Adds HttpReq when needed by native imports, which hides adapter plumbing from user code because
// WEB/REQ request types should feel ordinary.
static void ensure_builtin_httpreq(TypeTable *types) {
    if (type_find(types, "HttpReq")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("HttpReq");
    vec_push(&decl->fields, builtin_field("STR", "method", builtin_str_value("GET")));
    vec_push(&decl->fields, builtin_field("STR", "path", builtin_str_value("/")));
    vec_push(&decl->fields, builtin_field("STR", "body", builtin_str_value("")));
    vec_push(&decl->fields, builtin_field("MAP[STR,STR]", "headers", builtin_map_value()));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}

// Adds HttpRes when needed by native imports, which gives handlers a concrete response type
// because WEB/REQ returns must compile to C structs.
static void ensure_builtin_httpres(TypeTable *types) {
    if (type_find(types, "HttpRes")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("HttpRes");
    vec_push(&decl->fields, builtin_field("INT", "status", builtin_int_value(0)));
    vec_push(&decl->fields, builtin_field("STR", "body", builtin_str_value("")));
    vec_push(&decl->fields, builtin_field("MAP[STR,STR]", "headers", builtin_map_value()));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}

// Adds FileStat for FS.stat, which keeps filesystem metadata typed because the adapter returns a
// structured value.
static void ensure_builtin_filestat(TypeTable *types) {
    if (type_find(types, "FileStat")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("FileStat");
    vec_push(&decl->fields, builtin_field("STR", "path", builtin_str_value("")));
    vec_push(&decl->fields, builtin_field("BOL", "exists", builtin_bol_value(false)));
    vec_push(&decl->fields, builtin_field("BOL", "isFile", builtin_bol_value(false)));
    vec_push(&decl->fields, builtin_field("BOL", "isDir", builtin_bol_value(false)));
    vec_push(&decl->fields, builtin_field("INT", "size", builtin_int_value(0)));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}

// Adds ProcResult for PROC.run, which keeps process results typed because stdout, stderr, and exit
// code travel together.
static void ensure_builtin_procresult(TypeTable *types) {
    if (type_find(types, "ProcResult")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("ProcResult");
    vec_push(&decl->fields, builtin_field("INT", "code", builtin_int_value(0)));
    vec_push(&decl->fields, builtin_field("STR", "out", builtin_str_value("")));
    vec_push(&decl->fields, builtin_field("STR", "err", builtin_str_value("")));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}

// Adds SQL.DB, which represents a database handle as a typed value because the raw sqlite pointer
// stays inside the adapter table.
static void ensure_builtin_sqldb(TypeTable *types) {
    if (type_find(types, "SQL_DB")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("SQL_DB");
    vec_push(&decl->fields, builtin_field("INT", "handle", builtin_int_value(0)));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}

// Adds SQL.Exec, which records mutation results because SQL.exec needs rows affected and last
// insert id as one value.
static void ensure_builtin_sqlexec(TypeTable *types) {
    if (type_find(types, "SQL_Exec")) return;

    Token tok = {TK_MAP, xstrdup("MAP"), 1, 1};
    Node *decl = node_new(NK_TYPE_STRUCT, tok);
    decl->name = xstrdup("SQL_Exec");
    vec_push(&decl->fields, builtin_field("INT", "rows", builtin_int_value(0)));
    vec_push(&decl->fields, builtin_field("INT", "lastId", builtin_int_value(0)));

    TypeInfo info = {0};
    info.name = decl->name;
    info.kind = TYPEINFO_STRUCT;
    info.node = decl;
    type_push(types, info);
}
