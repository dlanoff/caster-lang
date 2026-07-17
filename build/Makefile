CC ?= cc
CASTER_C_STANDARD ?= c11
CFLAGS ?= -std=$(CASTER_C_STANDARD) -Wall -Wextra
CASTERC := output/caster
CASTERC_SOURCES = src/caster/driver/compiler.c src/caster/driver/common.c src/caster/lex/*.c src/caster/ast/*.c src/caster/parse/*.c src/caster/analyze/*.c src/caster/emit/*.c src/caster/adapters/*.c
BINDIR ?= $(HOME)/.local/bin
SRC ?= tests/authoritative.cast
CASTER_BUILD_DIR ?= .caster
OUT ?= $(CASTER_BUILD_DIR)/tests__authoritative.C
RUN_OUT ?= $(CASTER_BUILD_DIR)/tests__authoritative
REQ_HOST_OUT ?= $(CASTER_BUILD_DIR)/tests__req_host.C
REQ_WS_OUT ?= $(CASTER_BUILD_DIR)/tests__req_ws.C
SERVER_TEST_SRC ?= examples/server.cast
SERVER_TEST_OUT ?= $(CASTER_BUILD_DIR)/examples__server.C
SELF_HOSTING_EXE ?= $(CASTER_BUILD_DIR)/self_hosting__main
SELF_HOSTING_BAD_JSON ?= $(CASTER_BUILD_DIR)/self_hosting__bad.json
SELF_HOSTING_SELF1_C ?= $(CASTER_BUILD_DIR)/self_hosting__main.self1.C
SELF_HOSTING_SELF1 ?= $(CASTER_BUILD_DIR)/self_hosting__main.self1
SELF_HOSTING_SELF2_C ?= $(CASTER_BUILD_DIR)/self_hosting__main.self2.C
SELF_HOSTING_SELF2 ?= $(CASTER_BUILD_DIR)/self_hosting__main.self2
MUTATION_SYNTAX_JSON ?= $(CASTER_BUILD_DIR)/tests__mutationReadError.json
CASTER_RUN_SRC := $(word 2,$(MAKECMDGOALS))
CASTER_RUN_EXTRA_GOALS := $(filter %.cast,$(MAKECMDGOALS))
RUN_FIRST_GOAL := $(firstword $(MAKECMDGOALS))
RUN_SUBCOMMAND := $(word 2,$(MAKECMDGOALS))
RUN_SUBARG := $(word 3,$(MAKECMDGOALS))
RUN_BENCH_COMMANDS := install test caster fastapi go-http go-http-router h2o-get both
RUN_BENCH_MODE := $(if $(filter run,$(RUN_FIRST_GOAL)),$(filter $(RUN_BENCH_COMMANDS),$(RUN_SUBCOMMAND)),)
RUN_BENCH_EXTRA_GOALS := $(if $(RUN_BENCH_MODE),$(wordlist 2,$(words $(MAKECMDGOALS)),$(MAKECMDGOALS)),)

.PHONY: check-c-toolchain compiler caster install-caster uninstall-caster h2o cast check-json mutation-syntax-check run conway linked-list linked-list-nested collection-methods destructuring-test system-test buffer-test sql-test self-hosting-test self-hosting-c-sample self-hosting-json-diagnostics self-hosting-real req-host req-ws project-test web-test server-test test clean distclean

compiler: $(CASTERC)

check-c-toolchain:
	@CC="$(CC)" CFLAGS="$(CFLAGS)" CASTER_C_STANDARD="$(CASTER_C_STANDARD)" scripts/check-c-toolchain

$(CASTERC): $(CASTERC_SOURCES) | check-c-toolchain
	mkdir -p output
	$(CC) $(CFLAGS) src/caster/driver/compiler.c -o $(CASTERC)

h2o:
	cmake -S vendor/h2o -B vendor/h2o/build \
		-DCMAKE_BUILD_TYPE=Release \
		-DWITH_MRUBY=OFF \
		-DWITH_BROTLI=OFF \
		-DWITH_ZSTD=OFF \
		-DWITH_DTRACE=OFF \
		-DDISABLE_LIBUV=ON
	cmake --build vendor/h2o/build --target libh2o-evloop

ifeq ($(RUN_BENCH_MODE),)
caster: compiler
	@if [ -z "$(CASTER_RUN_SRC)" ]; then echo "usage: make caster ./foo.cast" >&2; exit 2; fi
	@scripts/caster "$(CASTER_RUN_SRC)"
endif

install-caster: check-c-toolchain compiler
	@if [ -e "$(BINDIR)/caster" ] && [ ! -L "$(BINDIR)/caster" ]; then echo "caster: error: $(BINDIR)/caster exists and is not a symlink" >&2; exit 1; fi
	@mkdir -p "$(BINDIR)"
	@ln -sfn "$(CURDIR)/caster" "$(BINDIR)/caster"
	@echo "installed $(BINDIR)/caster -> $(CURDIR)/caster"

uninstall-caster:
	@if [ -L "$(BINDIR)/caster" ]; then rm "$(BINDIR)/caster"; echo "removed $(BINDIR)/caster"; else echo "caster: no symlink at $(BINDIR)/caster"; fi

cast: compiler
	@scripts/caster "$(SRC)"

check-json: compiler
	$(CASTERC) check --json $(SRC) --repo-root $(CURDIR)

mutation-syntax-check: compiler
	@mkdir -p $(CASTER_BUILD_DIR)
	@if $(CASTERC) check --json tests/mutationReadError.cast --repo-root $(CURDIR) > $(MUTATION_SYNTAX_JSON) 2>&1; then \
		echo "expected mutationReadError.cast to fail" >&2; \
		exit 1; \
	fi
	@grep -q "mutation operator '\\*' is statement-only" $(MUTATION_SYNTAX_JSON)

run:
	@if [ -z "$(RUN_SUBCOMMAND)" ]; then \
		echo "usage: make run ./foo.cast" >&2; \
		echo "       make run install" >&2; \
		echo "       make run test fastapi|caster|go-http|go-http-router|h2o-get|both" >&2; \
		echo "       make run caster" >&2; \
		echo "       make run go-http" >&2; \
		echo "       make run go-http-router" >&2; \
		echo "       make run h2o-get" >&2; \
		exit 2; \
	fi
	@if [ "$(RUN_SUBCOMMAND)" = "install" ]; then \
		benchmarks/install.sh; \
	elif [ "$(RUN_SUBCOMMAND)" = "test" ]; then \
		if [ -z "$(RUN_SUBARG)" ]; then \
			echo "usage: make run test fastapi|caster|go-http|go-http-router|h2o-get|both" >&2; \
			exit 2; \
		fi; \
		benchmarks/run_benchmark.sh "$(RUN_SUBARG)"; \
	elif [ "$(RUN_SUBCOMMAND)" = "caster" ] || [ "$(RUN_SUBCOMMAND)" = "fastapi" ] || [ "$(RUN_SUBCOMMAND)" = "go-http" ] || [ "$(RUN_SUBCOMMAND)" = "go-http-router" ] || [ "$(RUN_SUBCOMMAND)" = "h2o-get" ] || [ "$(RUN_SUBCOMMAND)" = "both" ]; then \
		benchmarks/run_benchmark.sh "$(RUN_SUBCOMMAND)"; \
	else \
		scripts/caster "$(RUN_SUBCOMMAND)"; \
	fi

ifneq ($(CASTER_RUN_EXTRA_GOALS),)
.PHONY: $(CASTER_RUN_EXTRA_GOALS)
$(CASTER_RUN_EXTRA_GOALS):
	@:
endif

ifneq ($(RUN_BENCH_EXTRA_GOALS),)
.PHONY: $(RUN_BENCH_EXTRA_GOALS)
$(RUN_BENCH_EXTRA_GOALS):
	@:
endif

conway: compiler
	@scripts/caster tests/conway.cast

linked-list: compiler
	@scripts/caster tests/linkedList.cast

linked-list-nested: compiler
	@scripts/caster tests/linkedListNested.cast

collection-methods: compiler
	@scripts/caster tests/collectionMethods.cast

destructuring-test: compiler
	@scripts/caster tests/destructuring.cast

system-test: compiler
	@scripts/caster tests/system.cast

buffer-test: compiler
	@scripts/caster tests/buffer.cast

sql-test: compiler
	@scripts/caster tests/sql.cast

self-hosting-test: compiler
	@scripts/caster self_hosting/main.cast

self-hosting-c-sample: self-hosting-test
	@$(SELF_HOSTING_EXE) --emit-c > /tmp/caster-self-sample.c
	@$(CC) $(CFLAGS) /tmp/caster-self-sample.c -o /tmp/caster-self-sample
	@/tmp/caster-self-sample > /tmp/caster-self-sample.out
	@grep -qx '1' /tmp/caster-self-sample.out

self-hosting-json-diagnostics: self-hosting-test
	@$(SELF_HOSTING_EXE) check --json self_hosting/fixtures/bad.cast > $(SELF_HOSTING_BAD_JSON)
	@grep -q '"severity":"error"' $(SELF_HOSTING_BAD_JSON)
	@grep -q "unknown return type" $(SELF_HOSTING_BAD_JSON)

self-hosting-real: self-hosting-test
	@$(SELF_HOSTING_EXE) --emit-c self_hosting/main.cast > $(SELF_HOSTING_SELF1_C)
	@$(CC) $(CFLAGS) -x c $(SELF_HOSTING_SELF1_C) -o $(SELF_HOSTING_SELF1)
	@$(SELF_HOSTING_SELF1) --emit-c self_hosting/main.cast > $(SELF_HOSTING_SELF2_C)
	@$(CC) $(CFLAGS) -x c $(SELF_HOSTING_SELF2_C) -o $(SELF_HOSTING_SELF2)
	@cmp -s $(SELF_HOSTING_SELF1_C) $(SELF_HOSTING_SELF2_C)

req-host: compiler
	mkdir -p $(CASTER_BUILD_DIR)
	$(CASTERC) tests/req_host.cast -o $(REQ_HOST_OUT) --no-run --repo-root $(CURDIR)

req-ws: compiler
	mkdir -p $(CASTER_BUILD_DIR)
	$(CASTERC) tests/req_ws.cast -o $(REQ_WS_OUT) --no-run --repo-root $(CURDIR)

project-test: compiler
	@scripts/caster tests/project_test/main.cast

web-test: compiler
	@scripts/caster tests/project_test/web.cast

server-test: compiler
	mkdir -p $(CASTER_BUILD_DIR)
	$(CASTERC) $(SERVER_TEST_SRC) -o $(SERVER_TEST_OUT) --no-run --repo-root $(CURDIR)

ifeq ($(RUN_BENCH_MODE),)
test: h2o check-json mutation-syntax-check cast conway linked-list linked-list-nested collection-methods destructuring-test system-test buffer-test sql-test self-hosting-test self-hosting-c-sample self-hosting-json-diagnostics self-hosting-real req-host req-ws project-test web-test server-test
endif

clean:
	rm -rf output/*
	find . -name .caster -type d -prune -exec rm -rf {} +
	rm -rf benchmarks/.go benchmarks/.h2o
	rm -rf vendor/h2o/build
	rm -rf .tmp.dprobes.h

distclean: clean
	rm -rf benchmarks/.venv benchmarks/results
