// C Emitter
// ---------
//
// The emitter assumes analyzer.c already rejected invalid Caster. It walks the
// checked AST, collects only the runtime helpers the program needs, and writes
// one readable C file.

// ----------------------------- Emitter -----------------------------
#include "context.c"
#include "names.c"
#include "expr.c"
#include "stmt.c"
#include "types.c"
#include "runtime.c"
#include "program.c"
