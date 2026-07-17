// Caster buffer adapter.
//
// Included into generated C only when BUF.* is used. The user-facing Buffer is
// a tiny fixed MAP with an integer handle; the runtime table owns the growable
// native allocation behind that handle.

typedef struct {
    char *data;
    int64_t len;
    int64_t cap;
    bool alive;
} FLX_BUF_SLOT;

static FLX_BUF_SLOT *flx_buf_slots = NULL;
static int64_t flx_buf_slots_len = 0;
static int64_t flx_buf_slots_cap = 0;

// Looks up a Buffer slot, which validates handles before use because user-facing Buffer values
// should not expose raw pointers.
static FLX_UNUSED FLX_BUF_SLOT *flx_buf_slot(FLX_Buffer buffer) {
    int64_t index = buffer.handle - 1;
    if (index < 0 || index >= flx_buf_slots_len || !flx_buf_slots[index].alive) {
        fprintf(stderr, "caster: invalid Buffer handle\n");
        exit(1);
    }
    return &flx_buf_slots[index];
}

// Grows Buffer storage, which amortizes writes because compiler output and string building append
// repeatedly.
static FLX_UNUSED void flx_buf_reserve(FLX_BUF_SLOT *slot, int64_t needed) {
    if (needed <= slot->cap) return;
    int64_t next = slot->cap ? slot->cap : 64;
    while (next < needed) next *= 2;
    char *data = realloc(slot->data, (size_t)next + 1);
    if (!data) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    slot->data = data;
    slot->cap = next;
    if (slot->len == 0) slot->data[0] = 0;
}

// Appends raw bytes to a Buffer, which gives every typed write one shared path because formatting
// should not duplicate growth logic.
static FLX_UNUSED void flx_buf_append_bytes(FLX_BUF_SLOT *slot, const char *data, int64_t len) {
    if (len <= 0) return;
    flx_buf_reserve(slot, slot->len + len);
    memcpy(slot->data + slot->len, data, (size_t)len);
    slot->len += len;
    slot->data[slot->len] = 0;
}

// Allocates or reuses a Buffer slot, which keeps Buffer values cheap to copy because the runtime
// table owns mutable storage.
static FLX_UNUSED int64_t flx_buf_alloc_slot(void) {
    for (int64_t i = 0; i < flx_buf_slots_len; i++) {
        if (flx_buf_slots[i].alive) continue;
        flx_buf_slots[i] = (FLX_BUF_SLOT){0};
        flx_buf_slots[i].alive = true;
        return i + 1;
    }

    if (flx_buf_slots_len == flx_buf_slots_cap) {
        flx_buf_slots_cap = flx_buf_slots_cap ? flx_buf_slots_cap * 2 : 16;
        flx_buf_slots = realloc(flx_buf_slots, sizeof(FLX_BUF_SLOT) * (size_t)flx_buf_slots_cap);
        if (!flx_buf_slots) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    }

    int64_t index = flx_buf_slots_len++;
    flx_buf_slots[index] = (FLX_BUF_SLOT){0};
    flx_buf_slots[index].alive = true;
    return index + 1;
}

// Exposes BUF.new, which keeps growable text building behind a native adapter because self-hosted
// compiler code needs efficient output assembly.
static FLX_UNUSED FLX_Buffer BUF_new(void) {
    FLX_Buffer out = {0};
    out.handle = flx_buf_alloc_slot();
    return out;
}

// Exposes BUF.write_str, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_write_str(FLX_Buffer *buffer, FLX_STR value) {
    flx_buf_append_bytes(flx_buf_slot(*buffer), value.data, value.len);
}

// Exposes BUF.write_int, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_write_int(FLX_Buffer *buffer, int64_t value) {
    char text[64];
    int len = snprintf(text, sizeof(text), "%lld", (long long)value);
    if (len < 0) { fprintf(stderr, "caster: Buffer INT write failed\n"); exit(1); }
    flx_buf_append_bytes(flx_buf_slot(*buffer), text, len);
}

// Exposes BUF.write_flt, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_write_flt(FLX_Buffer *buffer, double value) {
    char text[128];
    int len = snprintf(text, sizeof(text), "%.17g", value);
    if (len < 0) { fprintf(stderr, "caster: Buffer FLT write failed\n"); exit(1); }
    flx_buf_append_bytes(flx_buf_slot(*buffer), text, len);
}

// Exposes BUF.write_bol, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_write_bol(FLX_Buffer *buffer, bool value) {
    if (value) flx_buf_append_bytes(flx_buf_slot(*buffer), "true", 4);
    else flx_buf_append_bytes(flx_buf_slot(*buffer), "false", 5);
}

// Exposes BUF.line_str, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_line_str(FLX_Buffer *buffer, FLX_STR value) {
    BUF_write_str(buffer, value);
    flx_buf_append_bytes(flx_buf_slot(*buffer), "\n", 1);
}

// Exposes BUF.line_int, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_line_int(FLX_Buffer *buffer, int64_t value) {
    BUF_write_int(buffer, value);
    flx_buf_append_bytes(flx_buf_slot(*buffer), "\n", 1);
}

// Exposes BUF.line_flt, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_line_flt(FLX_Buffer *buffer, double value) {
    BUF_write_flt(buffer, value);
    flx_buf_append_bytes(flx_buf_slot(*buffer), "\n", 1);
}

// Exposes BUF.line_bol, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_line_bol(FLX_Buffer *buffer, bool value) {
    BUF_write_bol(buffer, value);
    flx_buf_append_bytes(flx_buf_slot(*buffer), "\n", 1);
}

// Exposes BUF.toStr, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED FLX_STR BUF_toStr(FLX_Buffer buffer) {
    FLX_BUF_SLOT *slot = flx_buf_slot(buffer);
    FLX_STR out = flx_str_alloc(slot->len);
    if (slot->len > 0) memcpy(out.data, slot->data, (size_t)slot->len);
    return out;
}

// Exposes BUF.len, which keeps growable text building behind a native adapter because self-hosted
// compiler code needs efficient output assembly.
static FLX_UNUSED int64_t BUF_len(FLX_Buffer buffer) {
    return flx_buf_slot(buffer)->len;
}

// Exposes BUF.clear, which keeps growable text building behind a native adapter because
// self-hosted compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_clear(FLX_Buffer *buffer) {
    FLX_BUF_SLOT *slot = flx_buf_slot(*buffer);
    slot->len = 0;
    if (slot->data) slot->data[0] = 0;
}

// Exposes BUF.free, which keeps growable text building behind a native adapter because self-hosted
// compiler code needs efficient output assembly.
static FLX_UNUSED void BUF_free(FLX_Buffer *buffer) {
    FLX_BUF_SLOT *slot = flx_buf_slot(*buffer);
    free(slot->data);
    *slot = (FLX_BUF_SLOT){0};
    buffer->handle = 0;
}
