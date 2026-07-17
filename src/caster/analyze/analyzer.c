// Analyzer / Type Checker
// -----------------------
//
// The parser only proves that source text has valid shape. The analyzer gives
// that shape meaning: it resolves TYPE aliases, records struct fields, checks
// function boundaries, injects loop variables, and decides whether `x = value`
// is assignment or an inferred local declaration.

// ----------------------------- Type Environment -----------------------------

typedef enum {
    TYPEINFO_ALIAS,
    TYPEINFO_STRUCT
} TypeInfoKind;

typedef struct {
    char *name;
    TypeInfoKind kind;
    Node *node;
    char *target_raw;
    char *target;
    int resolving;
    bool open_runtime;
} TypeInfo;

typedef struct {
    TypeInfo *items;
    int len;
    int cap;
} TypeTable;

static TypeTable *g_types = NULL;
static TypeTable g_type_storage;
static char *set_type(Node *n, const char *type);

#include "types.c"
#include "symbols.c"
#include "type_rules.c"
#include "collections.c"
#include "statements.c"
#include "calls.c"
#include "expressions.c"
#include "program.c"
