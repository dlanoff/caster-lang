// Caster SQL adapter.
//
// Included into generated C only when SQL.* is used. The user-facing DB value
// is a small MAP handle; this runtime table owns the sqlite3 connection.

#include "vendor/sqlite/sqlite3.h"

typedef struct {
    sqlite3 *raw;
    bool alive;
} FLX_SQL_SLOT;

static FLX_SQL_SLOT *flx_sql_slots = NULL;
static int64_t flx_sql_slots_len = 0;
static int64_t flx_sql_slots_cap = 0;

// Copies FLX_STR into a null-terminated C string, which is needed because OS, sockets, H2O, and
// sqlite expect C APIs.
static FLX_UNUSED char *flx_sql_cstr(FLX_STR text) {
    char *out = malloc((size_t)text.len + 1);
    if (!out) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    for (int64_t i = 0; i < text.len; i++) out[i] = text.data[i];
    out[text.len] = 0;
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_UNUSED FLX_STR flx_sql_str_from_c(const char *text) {
    if (!text) text = "";
    int64_t len = (int64_t)strlen(text);
    FLX_STR out = flx_str_alloc(len);
    for (int64_t i = 0; i < len; i++) out.data[i] = text[i];
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_UNUSED FLX_STR flx_sql_str_from_bytes(const unsigned char *data, int64_t len) {
    FLX_STR out = flx_str_alloc(len);
    for (int64_t i = 0; i < len; i++) out.data[i] = (char)data[i];
    return out;
}

// Reports sqlite errors with operation context, which keeps database failures actionable because
// sqlite only stores the low-level message.
static FLX_UNUSED void flx_sql_fail(sqlite3 *db, const char *op) {
    fprintf(stderr, "caster: SQL.%s failed: %s\n", op, db ? sqlite3_errmsg(db) : "unknown sqlite error");
    exit(1);
}

// Looks up an owned sqlite handle slot, which keeps raw sqlite pointers out of Caster values
// because SQL.DB is just a typed handle.
static FLX_UNUSED FLX_SQL_SLOT *flx_sql_slot(FLX_SQL_DB db) {
    int64_t index = db.handle - 1;
    if (index < 0 || index >= flx_sql_slots_len || !flx_sql_slots[index].alive || !flx_sql_slots[index].raw) {
        fprintf(stderr, "caster: invalid SQL.DB handle\n");
        exit(1);
    }
    return &flx_sql_slots[index];
}

// Allocates a sqlite handle slot, which lets DB values copy safely because the adapter table owns
// the real connection.
static FLX_UNUSED int64_t flx_sql_alloc_slot(sqlite3 *raw) {
    for (int64_t i = 0; i < flx_sql_slots_len; i++) {
        if (flx_sql_slots[i].alive) continue;
        flx_sql_slots[i].raw = raw;
        flx_sql_slots[i].alive = true;
        return i + 1;
    }

    if (flx_sql_slots_len == flx_sql_slots_cap) {
        flx_sql_slots_cap = flx_sql_slots_cap ? flx_sql_slots_cap * 2 : 16;
        flx_sql_slots = realloc(flx_sql_slots, sizeof(FLX_SQL_SLOT) * (size_t)flx_sql_slots_cap);
        if (!flx_sql_slots) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    }

    int64_t index = flx_sql_slots_len++;
    flx_sql_slots[index].raw = raw;
    flx_sql_slots[index].alive = true;
    return index + 1;
}

// Prepares SQL text with sqlite, which centralizes statement errors because exec and query share
// the same setup.
static FLX_UNUSED sqlite3_stmt *flx_sql_prepare(sqlite3 *db, FLX_STR sql) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql.data, (int)sql.len, &stmt, NULL);
    if (rc != SQLITE_OK) flx_sql_fail(db, "prepare");
    return stmt;
}

// Binds one DYN parameter to sqlite, which makes mixed Caster param arrays work because sqlite
// needs concrete scalar bind calls.
static FLX_UNUSED void flx_sql_bind_value(sqlite3 *db, sqlite3_stmt *stmt, int index, FLX_OBJ value) {
    int rc = SQLITE_OK;
    switch (value.kind) {
        case FLX_OBJ_INT:
            rc = sqlite3_bind_int64(stmt, index, (sqlite3_int64)value.as.int_value);
            break;
        case FLX_OBJ_FLT:
            rc = sqlite3_bind_double(stmt, index, value.as.flt_value);
            break;
        case FLX_OBJ_BOL:
            rc = sqlite3_bind_int64(stmt, index, value.as.bol_value ? 1 : 0);
            break;
        case FLX_OBJ_STR:
            rc = sqlite3_bind_text(stmt, index, value.as.str_value.data, (int)value.as.str_value.len, SQLITE_TRANSIENT);
            break;
        case FLX_OBJ_NIL:
            rc = sqlite3_bind_null(stmt, index);
            break;
        default:
            fprintf(stderr, "caster: SQL params support INT, FLT, BOL, STR, or NUL\n");
            exit(1);
    }
    if (rc != SQLITE_OK) flx_sql_fail(db, "bind");
}

// Binds the DYN parameter array, which keeps SQL.exec/query simple because each call accepts the
// same parameter shape.
static FLX_UNUSED void flx_sql_bind_params(sqlite3 *db, sqlite3_stmt *stmt, FLX_OBJ params) {
    FLX_OBJARR arr = flx_obj_as_objarr(params);
    int expected = sqlite3_bind_parameter_count(stmt);
    if (arr.len != expected) {
        fprintf(stderr, "caster: SQL parameter count mismatch: expected %d, got %lld\n", expected, (long long)arr.len);
        exit(1);
    }
    for (int64_t i = 0; i < arr.len; i++) {
        flx_sql_bind_value(db, stmt, (int)i + 1, arr.data[i]);
    }
}

// Converts one sqlite column to DYN, which preserves query flexibility because result rows can
// contain mixed scalar values.
static FLX_UNUSED FLX_OBJ flx_sql_column_obj(sqlite3_stmt *stmt, int col) {
    switch (sqlite3_column_type(stmt, col)) {
        case SQLITE_INTEGER:
            return flx_obj_from_int((int64_t)sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT:
            return flx_obj_from_flt(sqlite3_column_double(stmt, col));
        case SQLITE_TEXT: {
            const unsigned char *text = sqlite3_column_text(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            return flx_obj_from_str(flx_sql_str_from_bytes(text, len));
        }
        case SQLITE_BLOB: {
            const unsigned char *blob = sqlite3_column_blob(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            return flx_obj_from_str(flx_sql_str_from_bytes(blob, len));
        }
        case SQLITE_NULL:
        default:
            return flx_obj_from_nil();
    }
}

// Converts one sqlite row to a DYN map, which lets user decoders turn query output into real MAP types at the boundary.
static FLX_UNUSED FLX_OBJ flx_sql_row_obj(sqlite3_stmt *stmt) {
    int cols = sqlite3_column_count(stmt);
    if (cols == 0) return flx_obj_from_objmap(flx_objmap_make(NULL, 0));

    FLX_OBJ_ENTRY *entries = malloc(sizeof(FLX_OBJ_ENTRY) * (size_t)cols);
    if (!entries) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    for (int col = 0; col < cols; col++) {
        entries[col].key = flx_sql_str_from_c(sqlite3_column_name(stmt, col));
        entries[col].value = flx_sql_column_obj(stmt, col);
    }

    FLX_OBJMAP row = flx_objmap_make(entries, cols);
    free(entries);
    return flx_obj_from_objmap(row);
}

// Exposes SQL.open, which keeps sqlite behind a typed adapter because Caster source should not
// depend on sqlite C APIs.
static FLX_UNUSED FLX_SQL_DB SQL_open(FLX_STR path) {
    char *raw_path = flx_sql_cstr(path);
    sqlite3 *raw = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    int rc = sqlite3_open_v2(raw_path, &raw, flags, NULL);
    free(raw_path);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "caster: SQL.open failed: %s\n", raw ? sqlite3_errmsg(raw) : "unknown sqlite error");
        if (raw) sqlite3_close(raw);
        exit(1);
    }

    FLX_SQL_DB out = {0};
    out.handle = flx_sql_alloc_slot(raw);
    return out;
}

// Exposes SQL.close, which keeps sqlite behind a typed adapter because Caster source should not
// depend on sqlite C APIs.
static FLX_UNUSED void SQL_close(FLX_SQL_DB *db) {
    if (!db || db->handle == 0) return;
    FLX_SQL_SLOT *slot = flx_sql_slot(*db);
    sqlite3_close(slot->raw);
    *slot = (FLX_SQL_SLOT){0};
    db->handle = 0;
}

// Exposes SQL.exec, which keeps sqlite behind a typed adapter because Caster source should not
// depend on sqlite C APIs.
static FLX_UNUSED FLX_SQL_Exec SQL_exec(FLX_SQL_DB *db, FLX_STR sql, FLX_OBJ params) {
    sqlite3 *raw = flx_sql_slot(*db)->raw;
    sqlite3_stmt *stmt = flx_sql_prepare(raw, sql);
    flx_sql_bind_params(raw, stmt, params);

    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        flx_sql_fail(raw, "exec");
    }
    sqlite3_finalize(stmt);

    FLX_SQL_Exec out = {0};
    out.rows = (int64_t)sqlite3_changes(raw);
    out.lastId = (int64_t)sqlite3_last_insert_rowid(raw);
    return out;
}

// Exposes SQL.query, which keeps sqlite behind a typed adapter because Caster source should not
// depend on sqlite C APIs.
static FLX_UNUSED FLX_OBJ SQL_query(FLX_SQL_DB *db, FLX_STR sql, FLX_OBJ params) {
    sqlite3 *raw = flx_sql_slot(*db)->raw;
    sqlite3_stmt *stmt = flx_sql_prepare(raw, sql);
    flx_sql_bind_params(raw, stmt, params);

    FLX_OBJARR rows = flx_objarr_make(NULL, 0);
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows = flx_objarr_add(rows, flx_sql_row_obj(stmt));
    }
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        flx_sql_fail(raw, "query");
    }
    sqlite3_finalize(stmt);

    return flx_obj_from_objarr(rows);
}
