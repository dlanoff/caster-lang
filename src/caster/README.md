# Caster Source Layout

The production C compiler is organized to mirror the self-hosting Caster
compiler as closely as the current C unity build allows:

- `driver/` contains CLI orchestration, file loading, module resolution, and shared C utilities.
- `lex/` contains source-to-token scanning.
- `parse/` contains token-to-AST parsing.
- `ast/` contains AST node helpers and the checked-tree printer.
- `analyze/` contains semantic analysis, type resolution, scope checks, and ownership checks.
- `emit/` contains the C emitter and generated-runtime helper emission.
- `adapters/` contains native adapter implementations used by emitted C, such as `BUF`, `REQ`, `SQL`, `OS`, `FS`, `PATH`, `IO`, and `PROC`.

The current C compiler is still a unity build: `driver/compiler.c` includes the
phase files into one translation unit. The directory layout is phase-oriented so
moving to separate compilation later does not require another conceptual
reshuffle.
