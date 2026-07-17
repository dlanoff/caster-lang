static void emit_open_runtime(FILE *f, EmitCtx *ctx) {
    if (!ctx->uses_open) return;

    fprintf(f,
        "/* open object runtime */\n"
        "typedef enum {\n"
        "    FLX_OBJ_INT,\n"
        "    FLX_OBJ_FLT,\n"
        "    FLX_OBJ_BOL,\n"
        "    FLX_OBJ_STR,\n"
        "    FLX_OBJ_NIL,\n"
        "    FLX_OBJ_MAP,\n"
        "    FLX_OBJ_ARR,\n"
        "    FLX_OBJ_BOX\n"
        "} FLX_OBJ_KIND;\n\n"
        "typedef struct FLX_OBJ FLX_OBJ;\n"
        "typedef struct {\n"
        "    FLX_OBJ* data;\n"
        "    int64_t len;\n"
        "    int64_t cap;\n"
        "} FLX_OBJARR;\n\n"
        "struct FLX_OBJ {\n"
        "    FLX_OBJ_KIND kind;\n"
        "    union {\n"
        "        int64_t int_value;\n"
        "        double flt_value;\n"
        "        bool bol_value;\n"
        "        FLX_STR str_value;\n"
        "        struct hashmap* map_value;\n"
        "        FLX_OBJARR arr_value;\n"
        "        void* ptr_value;\n"
        "    } as;\n"
        "};\n\n"
        "typedef struct {\n"
        "    FLX_STR key;\n"
        "    FLX_OBJ value;\n"
        "} FLX_OBJ_ENTRY;\n\n"
        "typedef struct {\n"
        "    struct hashmap* raw;\n"
        "} FLX_OBJMAP;\n\n"
        "static FLX_UNUSED uint64_t flx_objmap_hash(const void* item, uint64_t seed0, uint64_t seed1) {\n"
        "    const FLX_OBJ_ENTRY* entry = item;\n"
        "    return hashmap_sip(entry->key.data, (size_t)entry->key.len, seed0, seed1);\n"
        "}\n\n"
        "static FLX_UNUSED int flx_objmap_compare(const void* a, const void* b, void* udata) {\n"
        "    (void)udata;\n"
        "    const FLX_OBJ_ENTRY* left = a;\n"
        "    const FLX_OBJ_ENTRY* right = b;\n"
        "    if (left->key.len != right->key.len) return left->key.len < right->key.len ? -1 : 1;\n"
        "    for (int64_t i = 0; i < left->key.len; i++) {\n"
        "        if (left->key.data[i] != right->key.data[i]) return (unsigned char)left->key.data[i] < (unsigned char)right->key.data[i] ? -1 : 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_int(int64_t value) { FLX_OBJ out; out.kind = FLX_OBJ_INT; out.as.int_value = value; return out; }\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_flt(double value) { FLX_OBJ out; out.kind = FLX_OBJ_FLT; out.as.flt_value = value; return out; }\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_bol(bool value) { FLX_OBJ out; out.kind = FLX_OBJ_BOL; out.as.bol_value = value; return out; }\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_str(FLX_STR value) { FLX_OBJ out; out.kind = FLX_OBJ_STR; out.as.str_value = value; return out; }\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_nil(void) { FLX_OBJ out; out.kind = FLX_OBJ_NIL; out.as.ptr_value = NULL; return out; }\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_box_copy(const void* value, size_t size) {\n"
        "    FLX_OBJ out;\n"
        "    out.kind = FLX_OBJ_BOX;\n"
        "    out.as.ptr_value = malloc(size);\n"
        "    if (!out.as.ptr_value) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    memcpy(out.as.ptr_value, value, size);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED int64_t flx_obj_as_int(FLX_OBJ value) { if (value.kind != FLX_OBJ_INT) { fprintf(stderr, \"caster: object value is not INT\\n\"); exit(1); } return value.as.int_value; }\n"
        "static FLX_UNUSED double flx_obj_as_flt(FLX_OBJ value) { if (value.kind == FLX_OBJ_FLT) return value.as.flt_value; if (value.kind == FLX_OBJ_INT) return (double)value.as.int_value; fprintf(stderr, \"caster: object value is not FLT\\n\"); exit(1); }\n"
        "static FLX_UNUSED bool flx_obj_as_bol(FLX_OBJ value) { if (value.kind == FLX_OBJ_BOL) return value.as.bol_value; if (value.kind == FLX_OBJ_INT) return value.as.int_value != 0; fprintf(stderr, \"caster: object value is not BOL\\n\"); exit(1); }\n"
        "static FLX_UNUSED FLX_STR flx_obj_as_str(FLX_OBJ value) { if (value.kind != FLX_OBJ_STR) { fprintf(stderr, \"caster: object value is not STR\\n\"); exit(1); } return value.as.str_value; }\n"
        "static FLX_UNUSED FLX_OBJARR flx_obj_as_objarr(FLX_OBJ value) { if (value.kind != FLX_OBJ_ARR) { fprintf(stderr, \"caster: object value is not ARR\\n\"); exit(1); } return value.as.arr_value; }\n"
        "static FLX_UNUSED void* flx_obj_as_ref_nil(FLX_OBJ value) { if (value.kind != FLX_OBJ_NIL) { fprintf(stderr, \"caster: JSON null can only decode into REF values\\n\"); exit(1); } return NULL; }\n"
        "static FLX_UNUSED void* flx_obj_as_box(FLX_OBJ value) { if (value.kind != FLX_OBJ_BOX || !value.as.ptr_value) { fprintf(stderr, \"caster: object value is not boxed\\n\"); exit(1); } return value.as.ptr_value; }\n\n"
        "static FLX_UNUSED FLX_OBJARR flx_objarr_make(const FLX_OBJ* values, int64_t len) {\n"
        "    FLX_OBJARR arr;\n"
        "    arr.len = len;\n"
        "    arr.cap = len;\n"
        "    arr.data = NULL;\n"
        "    if (len > 0) {\n"
        "        arr.data = malloc(sizeof(FLX_OBJ) * (size_t)len);\n"
        "        if (!arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "        for (int64_t i = 0; i < len; i++) arr.data[i] = values[i];\n"
        "    }\n"
        "    return arr;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJARR flx_objarr_add(FLX_OBJARR src, FLX_OBJ value) {\n"
        "    FLX_OBJARR arr;\n"
        "    arr.len = src.len + 1;\n"
        "    arr.cap = arr.len;\n"
        "    arr.data = malloc(sizeof(FLX_OBJ) * (size_t)arr.len);\n"
        "    if (!arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    for (int64_t i = 0; i < src.len; i++) arr.data[i] = src.data[i];\n"
        "    arr.data[src.len] = value;\n"
        "    return arr;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_objarr_get(FLX_OBJARR src, int64_t index) {\n"
        "    if (index < 0 || index >= src.len) { fprintf(stderr, \"caster: array index out of range\\n\"); exit(1); }\n"
        "    return src.data[index];\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_objarr(FLX_OBJARR value) { FLX_OBJ out; out.kind = FLX_OBJ_ARR; out.as.arr_value = value; return out; }\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_new(void) {\n"
        "    FLX_OBJMAP map;\n"
        "    map.raw = hashmap_new(sizeof(FLX_OBJ_ENTRY), 0, 0, 0, flx_objmap_hash, flx_objmap_compare, NULL, NULL);\n"
        "    if (!map.raw) { fprintf(stderr, \"caster: hashmap allocation failed\\n\"); exit(1); }\n"
        "    return map;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_make(const FLX_OBJ_ENTRY* entries, int64_t len) {\n"
        "    FLX_OBJMAP map = flx_objmap_new();\n"
        "    for (int64_t i = 0; i < len; i++) hashmap_set(map.raw, &entries[i]);\n"
        "    return map;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_clone(FLX_OBJMAP src) {\n"
        "    FLX_OBJMAP dst = flx_objmap_new();\n"
        "    size_t iter = 0;\n"
        "    void* item = NULL;\n"
        "    while (src.raw && hashmap_iter(src.raw, &iter, &item)) hashmap_set(dst.raw, item);\n"
        "    return dst;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_add(FLX_OBJMAP src, FLX_STR key, FLX_OBJ value) {\n"
        "    FLX_OBJMAP dst = flx_objmap_clone(src);\n"
        "    FLX_OBJ_ENTRY entry = { key, value };\n"
        "    hashmap_set(dst.raw, &entry);\n"
        "    return dst;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_join(FLX_OBJMAP left, FLX_OBJMAP right) {\n"
        "    FLX_OBJMAP dst = flx_objmap_clone(left);\n"
        "    size_t iter = 0;\n"
        "    void* item = NULL;\n"
        "    while (right.raw && hashmap_iter(right.raw, &iter, &item)) hashmap_set(dst.raw, item);\n"
        "    return dst;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_objmap_get(FLX_OBJMAP src, FLX_STR key) {\n"
        "    FLX_OBJ_ENTRY probe;\n"
        "    probe.key = key;\n"
        "    FLX_OBJ_ENTRY* found = src.raw ? (FLX_OBJ_ENTRY*)hashmap_get(src.raw, &probe) : NULL;\n"
        "    if (!found) { fprintf(stderr, \"caster: missing object key\\n\"); exit(1); }\n"
        "    return found->value;\n"
        "}\n\n"
        "static FLX_UNUSED bool flx_objmap_has(FLX_OBJMAP src, FLX_STR key) {\n"
        "    FLX_OBJ_ENTRY probe;\n"
        "    probe.key = key;\n"
        "    return src.raw && hashmap_get(src.raw, &probe) != NULL;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_objmap_sub(FLX_OBJMAP src, FLX_STR key) {\n"
        "    FLX_OBJMAP dst = flx_objmap_clone(src);\n"
        "    FLX_OBJ_ENTRY probe;\n"
        "    probe.key = key;\n"
        "    hashmap_delete(dst.raw, &probe);\n"
        "    return dst;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_obj_from_objmap(FLX_OBJMAP value) { FLX_OBJ out; out.kind = FLX_OBJ_MAP; out.as.map_value = value.raw; return out; }\n"
        "static FLX_UNUSED FLX_OBJMAP flx_obj_as_objmap(FLX_OBJ value) { if (value.kind != FLX_OBJ_MAP) { fprintf(stderr, \"caster: object value is not MAP\\n\"); exit(1); } FLX_OBJMAP out; out.raw = value.as.map_value; return out; }\n"
        "static FLX_UNUSED void flx_drop_obj(FLX_OBJ value);\n"
        "static FLX_UNUSED void flx_drop_objarr(FLX_OBJARR arr) {\n"
        "    for (int64_t i = 0; i < arr.len; i++) flx_drop_obj(arr.data[i]);\n"
        "    free(arr.data);\n"
        "}\n\n"
        "static FLX_UNUSED void flx_drop_objmap(FLX_OBJMAP map) {\n"
        "    if (map.raw) hashmap_free(map.raw);\n"
        "}\n\n"
        "static FLX_UNUSED void flx_drop_obj(FLX_OBJ value) {\n"
        "    switch (value.kind) {\n"
        "        case FLX_OBJ_STR: flx_drop_str(value.as.str_value); break;\n"
        "        case FLX_OBJ_MAP: { FLX_OBJMAP map; map.raw = value.as.map_value; flx_drop_objmap(map); break; }\n"
        "        case FLX_OBJ_ARR: flx_drop_objarr(value.as.arr_value); break;\n"
        "        case FLX_OBJ_BOX: free(value.as.ptr_value); break;\n"
        "        case FLX_OBJ_INT:\n"
        "        case FLX_OBJ_FLT:\n"
        "        case FLX_OBJ_BOL:\n"
        "        case FLX_OBJ_NIL: break;\n"
        "    }\n"
        "}\n\n"
        "static FLX_UNUSED bool flx_obj_truthy(FLX_OBJ value) {\n"
        "    switch (value.kind) {\n"
        "        case FLX_OBJ_NIL: return false;\n"
        "        case FLX_OBJ_BOL: return value.as.bol_value;\n"
        "        case FLX_OBJ_INT: return value.as.int_value != 0;\n"
        "        case FLX_OBJ_FLT: return value.as.flt_value != 0.0;\n"
        "        case FLX_OBJ_STR: return value.as.str_value.len != 0;\n"
        "        case FLX_OBJ_MAP: return true;\n"
        "        case FLX_OBJ_ARR: return true;\n"
        "        case FLX_OBJ_BOX: return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n\n");
}

static void emit_json_runtime(FILE *f, EmitCtx *ctx) {
    if (!ctx->uses_json) return;

    fprintf(f,
        "/* JSON runtime */\n"
        "static FLX_UNUSED void flx_json_fail(const char* message) {\n"
        "    fprintf(stderr, \"caster: JSON decode failed: %%s\\n\", message);\n"
        "    exit(1);\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_json_copy_cstr(const char* text) {\n"
        "    if (!text) text = \"\";\n"
        "    int64_t len = (int64_t)strlen(text);\n"
        "    FLX_STR out = flx_str_alloc(len);\n"
        "    if (len > 0) memcpy(out.data, text, (size_t)len);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_json_from_cjson(const cJSON* item);\n"
        "static FLX_UNUSED FLX_OBJMAP flx_json_objmap_from_cjson(const cJSON* item);\n"
        "static FLX_UNUSED FLX_OBJARR flx_json_objarr_from_cjson(const cJSON* item);\n\n"
        "static FLX_UNUSED FLX_OBJ flx_json_number_from_cjson(const cJSON* item) {\n"
        "    double value = item->valuedouble;\n"
        "    if (value >= -9007199254740992.0 && value <= 9007199254740992.0) {\n"
        "        int64_t as_int = (int64_t)value;\n"
        "        if ((double)as_int == value) return flx_obj_from_int(as_int);\n"
        "    }\n"
        "    return flx_obj_from_flt(value);\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJARR flx_json_objarr_from_cjson(const cJSON* item) {\n"
        "    int64_t count = 0;\n"
        "    const cJSON* child = NULL;\n"
        "    cJSON_ArrayForEach(child, item) count++;\n"
        "    FLX_OBJARR out;\n"
        "    out.len = 0;\n"
        "    out.cap = count;\n"
        "    out.data = NULL;\n"
        "    if (count > 0) {\n"
        "        out.data = malloc(sizeof(FLX_OBJ) * (size_t)count);\n"
        "        if (!out.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    }\n"
        "    cJSON_ArrayForEach(child, item) out.data[out.len++] = flx_json_from_cjson(child);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_json_objmap_from_cjson(const cJSON* item) {\n"
        "    FLX_OBJMAP out = flx_objmap_new();\n"
        "    const cJSON* child = NULL;\n"
        "    cJSON_ArrayForEach(child, item) {\n"
        "        FLX_OBJ_ENTRY entry;\n"
        "        entry.key = flx_json_copy_cstr(child->string);\n"
        "        entry.value = flx_json_from_cjson(child);\n"
        "        hashmap_set(out.raw, &entry);\n"
        "    }\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_json_from_cjson(const cJSON* item) {\n"
        "    if (!item || cJSON_IsNull(item)) return flx_obj_from_nil();\n"
        "    if (cJSON_IsBool(item)) return flx_obj_from_bol(cJSON_IsTrue(item));\n"
        "    if (cJSON_IsNumber(item)) return flx_json_number_from_cjson(item);\n"
        "    if (cJSON_IsString(item)) return flx_obj_from_str(flx_json_copy_cstr(item->valuestring));\n"
        "    if (cJSON_IsArray(item)) return flx_obj_from_objarr(flx_json_objarr_from_cjson(item));\n"
        "    if (cJSON_IsObject(item)) return flx_obj_from_objmap(flx_json_objmap_from_cjson(item));\n"
        "    flx_json_fail(\"unsupported JSON value\");\n"
        "    return flx_obj_from_nil();\n"
        "}\n\n"
        "static FLX_UNUSED cJSON* flx_json_parse_cjson(FLX_STR json) {\n"
        "    const char* parse_end = NULL;\n"
        "    cJSON* root = cJSON_ParseWithLengthOpts(json.data, (size_t)json.len, &parse_end, 0);\n"
        "    if (!root) {\n"
        "        const char* detail = cJSON_GetErrorPtr();\n"
        "        if (detail && *detail) fprintf(stderr, \"caster: JSON decode failed near: %%.40s\\n\", detail);\n"
        "        else fprintf(stderr, \"caster: JSON decode failed\\n\");\n"
        "        exit(1);\n"
        "    }\n"
        "    const char* end = json.data + json.len;\n"
        "    while (parse_end && parse_end < end && isspace((unsigned char)*parse_end)) parse_end++;\n"
        "    if (parse_end != end) {\n"
        "        cJSON_Delete(root);\n"
        "        flx_json_fail(\"trailing input\");\n"
        "    }\n"
        "    return root;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJ flx_json_parse_dyn(FLX_STR json) {\n"
        "    cJSON* root = flx_json_parse_cjson(json);\n"
        "    FLX_OBJ out = flx_json_from_cjson(root);\n"
        "    cJSON_Delete(root);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_OBJMAP flx_json_parse_obj(FLX_STR json) {\n"
        "    cJSON* root = flx_json_parse_cjson(json);\n"
        "    if (!cJSON_IsObject(root)) {\n"
        "        cJSON_Delete(root);\n"
        "        flx_json_fail(\"expected object\");\n"
        "    }\n"
        "    FLX_OBJMAP out = flx_json_objmap_from_cjson(root);\n"
        "    cJSON_Delete(root);\n"
        "    return out;\n"
        "}\n\n");

    fprintf(f,
        "static FLX_UNUSED void flx_json_out_append(FLX_STR* out, const char* data, int64_t len) {\n"
        "    out->data = realloc(out->data, (size_t)(out->len + len + 1));\n"
        "    if (!out->data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    if (len > 0) memcpy(out->data + out->len, data, (size_t)len);\n"
        "    out->len += len;\n"
        "    out->data[out->len] = 0;\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_out_append_c(FLX_STR* out, char ch) {\n"
        "    flx_json_out_append(out, &ch, 1);\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_out_append_text(FLX_STR* out, const char* text) {\n"
        "    flx_json_out_append(out, text, (int64_t)strlen(text));\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_stringify_value(FLX_STR* out, FLX_OBJ value);\n\n"
        "static FLX_UNUSED void flx_json_stringify_string(FLX_STR* out, FLX_STR text) {\n"
        "    flx_json_out_append_c(out, '\\\"');\n"
        "    for (int64_t i = 0; i < text.len; i++) {\n"
        "        char ch = text.data[i];\n"
        "        if (ch == '\\\"') flx_json_out_append_text(out, \"\\\\\\\"\");\n"
        "        else if (ch == '\\\\') flx_json_out_append_text(out, \"\\\\\\\\\");\n"
        "        else if (ch == '\\n') flx_json_out_append_text(out, \"\\\\n\");\n"
        "        else if (ch == '\\t') flx_json_out_append_text(out, \"\\\\t\");\n"
        "        else flx_json_out_append_c(out, ch);\n"
        "    }\n"
        "    flx_json_out_append_c(out, '\\\"');\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_stringify_array(FLX_STR* out, FLX_OBJARR arr) {\n"
        "    flx_json_out_append_c(out, '[');\n"
        "    for (int64_t i = 0; i < arr.len; i++) {\n"
        "        if (i) flx_json_out_append_c(out, ',');\n"
        "        flx_json_stringify_value(out, arr.data[i]);\n"
        "    }\n"
        "    flx_json_out_append_c(out, ']');\n"
        "}\n\n"
        "typedef struct {\n"
        "    FLX_STR key;\n"
        "    FLX_OBJ value;\n"
        "} FLX_JSON_SORT_ENTRY;\n\n"
        "static int flx_json_compare_entries(const void* left_raw, const void* right_raw) {\n"
        "    const FLX_JSON_SORT_ENTRY* left = (const FLX_JSON_SORT_ENTRY*)left_raw;\n"
        "    const FLX_JSON_SORT_ENTRY* right = (const FLX_JSON_SORT_ENTRY*)right_raw;\n"
        "    int64_t min_len = left->key.len < right->key.len ? left->key.len : right->key.len;\n"
        "    int cmp = min_len > 0 ? memcmp(left->key.data, right->key.data, (size_t)min_len) : 0;\n"
        "    if (cmp != 0) return cmp;\n"
        "    if (left->key.len < right->key.len) return -1;\n"
        "    if (left->key.len > right->key.len) return 1;\n"
        "    return 0;\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_stringify_map(FLX_STR* out, FLX_OBJMAP map) {\n"
        "    flx_json_out_append_c(out, '{');\n"
        "    size_t count = map.raw ? hashmap_count(map.raw) : 0;\n"
        "    FLX_JSON_SORT_ENTRY* entries = count ? malloc(sizeof(FLX_JSON_SORT_ENTRY) * count) : NULL;\n"
        "    if (count && !entries) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    size_t iter = 0;\n"
        "    void* item = NULL;\n"
        "    size_t index = 0;\n"
        "    while (map.raw && hashmap_iter(map.raw, &iter, &item)) {\n"
        "        FLX_OBJ_ENTRY* entry = item;\n"
        "        entries[index++] = (FLX_JSON_SORT_ENTRY){ entry->key, entry->value };\n"
        "    }\n"
        "    if (count > 1) qsort(entries, count, sizeof(FLX_JSON_SORT_ENTRY), flx_json_compare_entries);\n"
        "    for (size_t i = 0; i < count; i++) {\n"
        "        if (i) flx_json_out_append_c(out, ',');\n"
        "        flx_json_stringify_string(out, entries[i].key);\n"
        "        flx_json_out_append_c(out, ':');\n"
        "        flx_json_stringify_value(out, entries[i].value);\n"
        "    }\n"
        "    free(entries);\n"
        "    flx_json_out_append_c(out, '}');\n"
        "}\n\n"
        "static FLX_UNUSED void flx_json_stringify_value(FLX_STR* out, FLX_OBJ value) {\n"
        "    char buf[64];\n"
        "    switch (value.kind) {\n"
        "        case FLX_OBJ_INT: snprintf(buf, sizeof(buf), \"%%lld\", (long long)value.as.int_value); flx_json_out_append_text(out, buf); break;\n"
        "        case FLX_OBJ_FLT: snprintf(buf, sizeof(buf), \"%%.17g\", value.as.flt_value); flx_json_out_append_text(out, buf); break;\n"
        "        case FLX_OBJ_BOL: flx_json_out_append_text(out, value.as.bol_value ? \"true\" : \"false\"); break;\n"
        "        case FLX_OBJ_STR: flx_json_stringify_string(out, value.as.str_value); break;\n"
        "        case FLX_OBJ_NIL: flx_json_out_append_text(out, \"null\"); break;\n"
        "        case FLX_OBJ_ARR: flx_json_stringify_array(out, value.as.arr_value); break;\n"
        "        case FLX_OBJ_MAP: { FLX_OBJMAP map; map.raw = value.as.map_value; flx_json_stringify_map(out, map); break; }\n"
        "        case FLX_OBJ_BOX: flx_json_out_append_text(out, \"null\"); break;\n"
        "    }\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_json_stringify(FLX_OBJ value) {\n"
        "    FLX_STR out = flx_str_alloc(0);\n"
        "    flx_json_stringify_value(&out, value);\n"
        "    return out;\n"
        "}\n\n");
}

static void emit_map_type_decls(FILE *f, EmitCtx *ctx) {
    if (!ctx->map_types.len) return;

    fprintf(f, "/* dynamic map types */\n");
    for (int i = 0; i < ctx->map_types.len; i++) {
        char *type = ctx->map_types.items[i];
        char *key_type = map_key_type(type);
        if (!type_eq(key_type, "STR")) continue;

        fprintf(f,
            "typedef struct {\n"
            "    struct hashmap* raw;\n"
            "} %s;\n\n",
            ctype(type));
    }
}

static void emit_map_runtimes(FILE *f, EmitCtx *ctx) {
    if (!ctx->map_types.len) return;

    fprintf(f, "/* dynamic map runtime */\n");
    for (int i = 0; i < ctx->map_types.len; i++) {
        char *type = ctx->map_types.items[i];
        char *key_type = map_key_type(type);
        char *value_type = map_value_type(type);

        if (!type_eq(key_type, "STR")) {
            fprintf(f, "/* unsupported non-STR key map: %s */\n", type);
            continue;
        }

        char *map_ct = ctype(type);
        char *entry_ct = map_entry_ctype(type);
        char *hash_fn = map_fn(type, "hash");
        char *cmp_fn = map_fn(type, "compare");
        char *new_fn = map_fn(type, "new");
        char *make_fn = map_fn(type, "make");
        char *clone_fn = map_fn(type, "clone");
        char *add_fn = map_fn(type, "add");
        char *join_fn = map_fn(type, "join");
        char *get_fn = map_fn(type, "get");
        char *has_fn = map_fn(type, "has");
        char *sub_fn = map_fn(type, "sub");
        char *drop_fn = map_fn(type, "drop");
        char *drop_entry_fn = map_fn(type, "drop_entry");
        char *set_copy_fn = map_fn(type, "set_copy");
        bool value_is_str = type_eq(value_type, "STR");
        char *copy_value_expr = value_is_str ? xstrdup("flx_str_copy(value)") : xstrdup("value");
        char *drop_value_stmt = value_is_str ? strf("    flx_drop_str(entry->value);\n") : xstrdup("");

        fprintf(f,
            "typedef struct {\n"
            "    FLX_STR key;\n"
            "    %s value;\n"
            "} %s;\n\n"
            "static FLX_UNUSED uint64_t %s(const void* item, uint64_t seed0, uint64_t seed1) {\n"
            "    const %s* entry = item;\n"
            "    return hashmap_sip(entry->key.data, (size_t)entry->key.len, seed0, seed1);\n"
            "}\n\n"
            "static FLX_UNUSED int %s(const void* a, const void* b, void* udata) {\n"
            "    (void)udata;\n"
            "    const %s* left = a;\n"
            "    const %s* right = b;\n"
            "    if (left->key.len != right->key.len) return left->key.len < right->key.len ? -1 : 1;\n"
            "    for (int64_t i = 0; i < left->key.len; i++) {\n"
            "        if (left->key.data[i] != right->key.data[i]) return (unsigned char)left->key.data[i] < (unsigned char)right->key.data[i] ? -1 : 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(void) {\n"
            "    %s map;\n"
            "    map.raw = hashmap_new(sizeof(%s), 0, 0, 0, %s, %s, NULL, NULL);\n"
            "    if (!map.raw) { fprintf(stderr, \"caster: hashmap allocation failed\\n\"); exit(1); }\n"
            "    return map;\n"
            "}\n\n"
            "static FLX_UNUSED void %s(%s* entry) {\n"
            "    if (!entry) return;\n"
            "    flx_drop_str(entry->key);\n"
            "%s"
            "}\n\n"
            "static FLX_UNUSED void %s(%s* map, FLX_STR key, %s value) {\n"
            "    %s entry = { flx_str_copy(key), %s };\n"
            "    const %s* replaced = (%s*)hashmap_set(map->raw, &entry);\n"
            "    if (replaced) %s((%s*)replaced);\n"
            "    if (hashmap_oom(map->raw)) { fprintf(stderr, \"caster: hashmap allocation failed\\n\"); exit(1); }\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(const %s* entries, int64_t len) {\n"
            "    %s map = %s();\n"
            "    for (int64_t i = 0; i < len; i++) %s(&map, entries[i].key, entries[i].value);\n"
            "    return map;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s src) {\n"
            "    %s dst = %s();\n"
            "    size_t iter = 0;\n"
            "    void* item = NULL;\n"
            "    while (src.raw && hashmap_iter(src.raw, &iter, &item)) {\n"
            "        %s* entry = item;\n"
            "        %s(&dst, entry->key, entry->value);\n"
            "    }\n"
            "    return dst;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s src, FLX_STR key, %s value) {\n"
            "    %s dst = %s(src);\n"
            "    %s(&dst, key, value);\n"
            "    return dst;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s left, %s right) {\n"
            "    %s dst = %s(left);\n"
            "    size_t iter = 0;\n"
            "    void* item = NULL;\n"
            "    while (right.raw && hashmap_iter(right.raw, &iter, &item)) {\n"
            "        %s* entry = item;\n"
            "        %s(&dst, entry->key, entry->value);\n"
            "    }\n"
            "    return dst;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s src, FLX_STR key) {\n"
            "    %s probe;\n"
            "    probe.key = key;\n"
            "    %s* found = src.raw ? (%s*)hashmap_get(src.raw, &probe) : NULL;\n"
            "    if (!found) { fprintf(stderr, \"caster: missing map key\\n\"); exit(1); }\n"
            "    return found->value;\n"
            "}\n\n"
            "static FLX_UNUSED bool %s(%s src, FLX_STR key) {\n"
            "    %s probe;\n"
            "    probe.key = key;\n"
            "    return src.raw && hashmap_get(src.raw, &probe) != NULL;\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s src, FLX_STR key) {\n"
            "    %s dst = %s(src);\n"
            "    %s probe;\n"
            "    probe.key = key;\n"
            "    const %s* removed = (%s*)hashmap_delete(dst.raw, &probe);\n"
            "    if (removed) %s((%s*)removed);\n"
            "    return dst;\n"
            "}\n\n"
            "static FLX_UNUSED void %s(%s value) {\n"
            "    if (value.raw) {\n"
            "        size_t iter = 0;\n"
            "        void* item = NULL;\n"
            "        while (hashmap_iter(value.raw, &iter, &item)) %s((%s*)item);\n"
            "        hashmap_free(value.raw);\n"
            "    }\n"
            "}\n\n",
            ctype(value_type), entry_ct,
            hash_fn, entry_ct,
            cmp_fn, entry_ct, entry_ct,
            map_ct, new_fn,
            map_ct, entry_ct, hash_fn, cmp_fn,
            drop_entry_fn, entry_ct,
            drop_value_stmt,
            set_copy_fn, map_ct, ctype(value_type),
            entry_ct, copy_value_expr,
            entry_ct, entry_ct,
            drop_entry_fn, entry_ct,
            map_ct, make_fn, entry_ct,
            map_ct, new_fn,
            set_copy_fn,
            map_ct, clone_fn, map_ct,
            map_ct, new_fn,
            entry_ct,
            set_copy_fn,
            map_ct, add_fn, map_ct, ctype(value_type),
            map_ct, clone_fn,
            set_copy_fn,
            map_ct, join_fn, map_ct, map_ct,
            map_ct, clone_fn,
            entry_ct,
            set_copy_fn,
            ctype(value_type), get_fn, map_ct,
            entry_ct,
            entry_ct,
            entry_ct,
            has_fn, map_ct,
            entry_ct,
            map_ct, sub_fn, map_ct,
            map_ct, clone_fn,
            entry_ct,
            entry_ct, entry_ct,
            drop_entry_fn, entry_ct,
            drop_fn, map_ct,
            drop_entry_fn, entry_ct);
    }
}

static void emit_open_box_helper(FILE *f, PtrVec *emitted, const char *type) {
    if (!type || is_obj_scalar_type(type) || type_eq(type, "NUL")) return;
    if (type_in_vec(emitted, type)) return;
    vec_push(emitted, xstrdup(type));

    char *ct = ctype(type);
    char *suffix = lower_mangle(type);
    fprintf(f,
        "static FLX_UNUSED FLX_OBJ flx_obj_from_%s(%s value) {\n"
        "    return flx_obj_box_copy(&value, sizeof(%s));\n"
        "}\n\n"
        "static FLX_UNUSED %s flx_obj_as_%s(FLX_OBJ value) {\n"
        "    return *(%s*)flx_obj_as_box(value);\n"
        "}\n\n",
        suffix, ct,
        ct,
        ct, suffix,
        ct);
}

static void emit_open_box_helpers(FILE *f, EmitCtx *ctx) {
    if (!ctx->uses_open) return;

    PtrVec emitted = {0};
    fprintf(f, "/* open object typed boxes */\n");
    for (int i = 0; i < ctx->array_types.len; i++) emit_open_box_helper(f, &emitted, ctx->array_types.items[i]);
    for (int i = 0; i < ctx->map_types.len; i++) emit_open_box_helper(f, &emitted, ctx->map_types.items[i]);
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind == TYPEINFO_STRUCT) emit_open_box_helper(f, &emitted, info->name);
    }
    fprintf(f, "\n");
}

static char *emit_json_obj_as_type(const char *type, const char *obj_expr) {
    if (is_struct_type(type) || is_map_type(type)) {
        return strf("%s(flx_obj_as_objmap(%s))", json_decode_fn(type), obj_expr);
    }
    if (is_array_type(type)) {
        return strf("%s(flx_obj_as_objarr(%s))", json_decode_fn(type), obj_expr);
    }
    if (is_ref_type(type)) {
        return strf("(%s)flx_obj_as_ref_nil(%s)", ctype(type), obj_expr);
    }
    if (type_eq(type, "STR")) return strf("flx_str_copy(flx_obj_as_str(%s))", obj_expr);
    return strf("%s(%s)", obj_as_fn(type), obj_expr);
}

static void emit_json_decode_helpers(FILE *f, EmitCtx *ctx) {
    if (!ctx->json_decode_types.len) return;

    fprintf(f, "/* JSON decode helpers */\n");
    for (int i = 0; i < ctx->json_decode_types.len; i++) {
        char *type = ctx->json_decode_types.items[i];
        if (is_array_type(type)) {
            fprintf(f, "static %s %s(FLX_OBJARR src);\n", ctype(type), json_decode_fn(type));
        } else if (is_struct_type(type) || is_map_type(type)) {
            fprintf(f, "static %s %s(FLX_OBJMAP src);\n", ctype(type), json_decode_fn(type));
        }
    }
    fprintf(f, "\n");

    for (int i = 0; i < ctx->json_decode_types.len; i++) {
        char *type = ctx->json_decode_types.items[i];

        if (is_struct_type(type)) {
            TypeInfo *info = type_find(g_types, type);
            fprintf(f, "static %s %s(FLX_OBJMAP src) {\n", ctype(type), json_decode_fn(type));
            fprintf(f, "    %s out = %s();\n", ctype(type), emit_named_value_fn_name(type));
            for (int j = 0; info && j < info->node->fields.len; j++) {
                Node *field = info->node->fields.items[j];
                char *key = emit_key_literal(field->name);
                if (is_open_struct_type(type)) {
                    fprintf(f,
                        "    if (flx_objmap_has(src, %s)) out = %s;\n",
                        key,
                        emit_obj_set_typed("out", key, field->declared_type,
                            emit_json_obj_as_type(field->declared_type, strf("flx_objmap_get(src, %s)", key))));
                } else {
                    fprintf(f,
                        "    if (flx_objmap_has(src, %s)) out.%s = %s;\n",
                        key,
                        field->name,
                        emit_json_obj_as_type(field->declared_type, strf("flx_objmap_get(src, %s)", key)));
                }
            }
            fprintf(f, "    return out;\n}\n\n");
            continue;
        }

        if (is_map_type(type)) {
            char *key_type = map_key_type(type);
            char *value_type = map_value_type(type);
            if (!type_eq(key_type, "STR")) {
                fprintf(f, "/* JSON decode unsupported non-STR key map: %s */\n", type);
                continue;
            }
            fprintf(f, "static %s %s(FLX_OBJMAP src) {\n", ctype(type), json_decode_fn(type));
            fprintf(f, "    %s out = %s(NULL, 0);\n", ctype(type), map_fn(type, "make"));
            fprintf(f, "    size_t iter = 0;\n");
            fprintf(f, "    void* item = NULL;\n");
            fprintf(f, "    while (src.raw && hashmap_iter(src.raw, &iter, &item)) {\n");
            fprintf(f, "        FLX_OBJ_ENTRY* entry = item;\n");
            fprintf(f, "        out = %s(out, entry->key, %s);\n",
                map_fn(type, "add"),
                emit_json_obj_as_type(value_type, "entry->value"));
            fprintf(f, "    }\n");
            fprintf(f, "    return out;\n}\n\n");
            continue;
        }

        if (is_array_type(type)) {
            char *element_type = array_elem_type(type);
            fprintf(f, "static %s %s(FLX_OBJARR src) {\n", ctype(type), json_decode_fn(type));
            fprintf(f, "    %s out = %s(NULL, 0);\n", ctype(type), arr_make(type));
            fprintf(f, "    for (int64_t i = 0; i < src.len; i++) {\n");
            fprintf(f, "        out = %s(out, %s);\n",
                arr_add(type),
                emit_json_obj_as_type(element_type, "src.data[i]"));
            fprintf(f, "    }\n");
            fprintf(f, "    return out;\n}\n\n");
        }
    }
}
