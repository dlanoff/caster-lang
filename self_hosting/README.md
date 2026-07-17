# Caster Self Hosting

This folder contains the Caster-written compiler/transpiler workstream. It is
not wired into the production `caster` command yet; the current C implementation
under `src/caster/` remains the compiler of record.

The current phase gives the Caster implementation a real parser and split
emission phases without double-nesting the source tree:

- `main.cast` is the CLI entrypoint.
- `driver/` contains diagnostics, source loading, and the phase pipeline.
- `lex/` contains token declarations and lexing.
- `parse/` contains parser orchestration and recursive parse modules.
- `ast/` contains the shared syntax tree declarations.
- `features/` contains delimiter and syntax feature registration.
- `loading/` contains local import graph loading and module tagging.
- `analyze/` contains the first self-hosted analyzer pass.
- `adapters/` contains the generated-C adapter emitters for core runtime,
  `BUF`, `FS`, `IO`, `OS`, and `PATH`.
- `emit/` contains C emission plus the summary emitter used while developing
  the front end.

The phase-2 parser now parses function bodies instead of storing only body text.
The implementation now includes a real import-aware analyzer slice, JSON
diagnostic output for editor integration, and enough runtime/emitter surface to
compile the default sample and the self-hosting compiler itself. The real
self-hosting loop generates C for `self_hosting/main.cast`, compiles that
generated compiler, uses it to emit itself again, and verifies the two generated
C files are byte-identical.

The next self-hosting phase should deepen production parity: port the remaining
production analyzer checks, close the remaining runtime/emitter gaps, clean the
generated-C warnings, and compare fixtures against the current C compiler before
making the Caster implementation the compiler of record.

Run the skeleton from the repository with:

```sh
make -f build/Makefile self-hosting-test
```

To exercise the first generated-C path:

```sh
make -f build/Makefile self-hosting-c-sample
```

To exercise the real self-hosting loop:

```sh
make -f build/Makefile self-hosting-real
```

To exercise the self-hosted JSON diagnostics path:

```sh
make -f build/Makefile self-hosting-json-diagnostics
.caster/self_hosting__main check --json self_hosting/main.cast
```
