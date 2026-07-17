#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

target=${1:-}
duration=${2:-30s}
connections=${3:-128}
threads=${4:-4}

host=${BENCH_HOST:-127.0.0.1}
port=${BENCH_PORT:-8000}
warmup=${BENCH_WARMUP:-5s}
hey_route=${BENCH_HEY_ROUTE:-health}
hey_timeout=${BENCH_HEY_TIMEOUT:-20}
base_url="http://$host:$port"
wrk_script="$repo_dir/benchmarks/wrk_bank_mixed.lua"
results_dir="$repo_dir/benchmarks/results"
server_pid=""
server_log=""

if [ -n "${BENCH_TOOL:-}" ]; then
    bench_tool=$BENCH_TOOL
elif [ "$target" = "h2o-get" ]; then
    bench_tool=hey
else
    bench_tool=wrk
fi

if [ -x "$repo_dir/benchmarks/.venv/bin/python" ]; then
    bench_python="$repo_dir/benchmarks/.venv/bin/python"
else
    bench_python=python3
fi

usage() {
    echo "usage: benchmarks/run_benchmark.sh caster|fastapi|go-http|go-http-router|h2o-get|both [duration] [connections] [threads]" >&2
    echo "example: benchmarks/run_benchmark.sh go-http 30s 128 4" >&2
    echo "set BENCH_TOOL=wrk|hey to choose the load generator" >&2
}

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "benchmark: missing command: $1" >&2
        return 1
    fi
}

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" >/dev/null 2>&1 || true
        server_pid=""
    fi
}

print_server_log_tail() {
    if [ -n "$server_log" ] && [ -f "$server_log" ]; then
        echo "benchmark: server log tail ($server_log)" >&2
        tail -n 80 "$server_log" >&2
    fi
}

ensure_server_alive() {
    if [ -n "$server_pid" ] && kill -0 "$server_pid" >/dev/null 2>&1; then
        return 0
    fi

    echo "benchmark: $1 server exited unexpectedly" >&2
    print_server_log_tail
    return 1
}

health_check() {
    python3 - "$base_url/health" <<'PY' >/dev/null 2>&1
import sys
import urllib.request

try:
    with urllib.request.urlopen(sys.argv[1], timeout=0.25) as response:
        raise SystemExit(0 if response.status < 500 else 1)
except Exception:
    raise SystemExit(1)
PY
}

wait_for_health() {
    i=0

    while [ "$i" -lt 300 ]; do
        if [ -n "$server_pid" ] && ! kill -0 "$server_pid" >/dev/null 2>&1; then
            echo "benchmark: server exited before becoming healthy at $base_url/health" >&2
            print_server_log_tail
            return 1
        fi

        if health_check; then
            return 0
        fi

        sleep 0.1
        i=$((i + 1))
    done

    echo "benchmark: server did not become healthy at $base_url/health" >&2
    return 1
}

ensure_port_free() {
    if health_check; then
        echo "benchmark: $base_url/health is already responding; stop that server first" >&2
        return 1
    fi
}

check_fastapi_deps() {
    "$bench_python" - <<'PY' >/dev/null 2>&1
import fastapi
import uvicorn
PY
}

start_caster() {
    ensure_port_free
    mkdir -p "$results_dir"
    server_log="$results_dir/caster_server_$(date +%Y%m%d-%H%M%S).log"

    (
        cd "$repo_dir"
        exec "$repo_dir/caster" "$repo_dir/benchmarks/bankserver.cast"
    ) >"$server_log" 2>&1 &

    server_pid=$!
    wait_for_health || {
        print_server_log_tail
        return 1
    }
    ensure_server_alive caster
}

start_fastapi() {
    ensure_port_free
    mkdir -p "$results_dir"
    server_log="$results_dir/fastapi_server_$(date +%Y%m%d-%H%M%S).log"

    if ! check_fastapi_deps; then
        echo "benchmark: missing FastAPI deps" >&2
        echo "install with: python3 -m venv benchmarks/.venv" >&2
        echo "then: . benchmarks/.venv/bin/activate" >&2
        echo "then: python -m pip install -r benchmarks/requirements.txt" >&2
        return 1
    fi

    (
        cd "$repo_dir"
        exec "$bench_python" -m uvicorn benchmarks.fastapi_bankserver:app --host "$host" --port "$port" --workers 1 --log-level warning
    ) >"$server_log" 2>&1 &

    server_pid=$!
    wait_for_health || {
        print_server_log_tail
        return 1
    }
    ensure_server_alive fastapi
}

start_go_http_mode() {
    label=$1
    router_mode=$2

    ensure_port_free
    mkdir -p "$results_dir" "$repo_dir/benchmarks/.go"
    server_log="$results_dir/${label}_server_$(date +%Y%m%d-%H%M%S).log"
    build_log="$results_dir/${label}_build_$(date +%Y%m%d-%H%M%S).log"
    binary="$repo_dir/benchmarks/.go/go-http-bankserver"

    if ! need_cmd go; then
        echo "benchmark: Go is required for the $label target" >&2
        echo "install Go first, for example: brew install go" >&2
        echo "on Debian/Ubuntu try: sudo apt-get install golang-go" >&2
        return 127
    fi

    if ! go build -o "$binary" "$repo_dir/benchmarks/go_http_bankserver.go" >"$build_log" 2>&1; then
        echo "benchmark: $label build failed" >&2
        cat "$build_log" >&2
        return 1
    fi

    echo "benchmark: $label using GOMAXPROCS=1"

    (
        cd "$repo_dir"
        GOMAXPROCS=1 BENCH_GO_ROUTER="$router_mode" BENCH_HOST="$host" BENCH_PORT="$port" exec "$binary"
    ) >"$server_log" 2>&1 &

    server_pid=$!
    wait_for_health || {
        print_server_log_tail
        return 1
    }
    ensure_server_alive "$label"
}

start_go_http() {
    start_go_http_mode go-http direct
}

start_go_http_router() {
    start_go_http_mode go-http-router table
}

cmake_cache_value() {
    key=$1
    cache=$2

    awk -F= -v key="$key" 'index($1, key ":") == 1 { print $2; exit }' "$cache"
}

start_h2o_get() {
    ensure_port_free
    mkdir -p "$results_dir" "$repo_dir/benchmarks/.h2o"
    server_log="$results_dir/h2o-get_server_$(date +%Y%m%d-%H%M%S).log"
    configure_log="$results_dir/h2o-get_configure_$(date +%Y%m%d-%H%M%S).log"
    build_log="$results_dir/h2o-get_build_$(date +%Y%m%d-%H%M%S).log"
    h2o_src="$repo_dir/vendor/h2o"
    h2o_build="$repo_dir/benchmarks/.h2o/build"
    binary="$repo_dir/benchmarks/.h2o/h2o-get-server"

    if [ ! -d "$h2o_src" ]; then
        echo "benchmark: missing H2O source at $h2o_src" >&2
        return 1
    fi

    if ! need_cmd cmake; then
        echo "benchmark: cmake is required for the h2o-get target" >&2
        return 127
    fi

    if [ ! -f "$h2o_build/CMakeCache.txt" ]; then
        if ! cmake -S "$h2o_src" -B "$h2o_build" \
            -DCMAKE_BUILD_TYPE=Release \
            -DWITH_MRUBY=OFF \
            -DWITH_BROTLI=OFF \
            -DWITH_ZSTD=OFF \
            -DDISABLE_LIBUV=ON >"$configure_log" 2>&1; then
            echo "benchmark: h2o-get H2O configure failed" >&2
            cat "$configure_log" >&2
            return 1
        fi
    fi

    if ! cmake --build "$h2o_build" --target libh2o-evloop --config Release >"$build_log" 2>&1; then
        echo "benchmark: h2o-get H2O build failed" >&2
        cat "$build_log" >&2
        return 1
    fi

    h2o_lib="$h2o_build/libh2o-evloop.a"
    if [ ! -f "$h2o_lib" ]; then
        echo "benchmark: expected H2O library not found at $h2o_lib" >&2
        return 1
    fi

    openssl_include=$(cmake_cache_value OPENSSL_INCLUDE_DIR "$h2o_build/CMakeCache.txt")
    openssl_ssl=$(cmake_cache_value OPENSSL_SSL_LIBRARY "$h2o_build/CMakeCache.txt")
    openssl_crypto=$(cmake_cache_value OPENSSL_CRYPTO_LIBRARY "$h2o_build/CMakeCache.txt")
    zlib_library=$(cmake_cache_value ZLIB_LIBRARY_RELEASE "$h2o_build/CMakeCache.txt")
    if [ -z "$zlib_library" ]; then
        zlib_library=$(cmake_cache_value ZLIB_LIBRARY "$h2o_build/CMakeCache.txt")
    fi

    if [ -z "$openssl_include" ] || [ -z "$openssl_ssl" ] || [ -z "$openssl_crypto" ]; then
        echo "benchmark: H2O build did not expose OpenSSL paths in CMakeCache.txt" >&2
        return 1
    fi

    link_zlib=
    if [ -n "$zlib_library" ]; then
        link_zlib=$zlib_library
    fi

    if ! cc -O3 -DNDEBUG -DH2O_USE_LIBUV=0 -pthread \
        -I"$h2o_src/include" \
        -I"$h2o_src/deps/cloexec" \
        -I"$h2o_src/deps/golombset" \
        -I"$h2o_src/deps/hiredis" \
        -I"$h2o_src/deps/libgkc" \
        -I"$h2o_src/deps/libyrmcds" \
        -I"$h2o_src/deps/klib" \
        -I"$h2o_src/deps/neverbleed" \
        -I"$h2o_src/deps/picohttpparser" \
        -I"$h2o_src/deps/picotest" \
        -I"$h2o_src/deps/picotls/deps/cifra/src/ext" \
        -I"$h2o_src/deps/picotls/deps/cifra/src" \
        -I"$h2o_src/deps/picotls/deps/micro-ecc" \
        -I"$h2o_src/deps/picotls/include" \
        -I"$h2o_src/deps/quicly/include" \
        -I"$h2o_src/deps/yaml/include" \
        -I"$h2o_src/deps/yoml" \
        -I"$h2o_build" \
        -I"$openssl_include" \
        "$repo_dir/benchmarks/h2o_get_server.c" \
        "$h2o_lib" "$openssl_ssl" "$openssl_crypto" $link_zlib -lm -pthread \
        -o "$binary" >"$build_log" 2>&1; then
        echo "benchmark: h2o-get server build failed" >&2
        cat "$build_log" >&2
        return 1
    fi

    echo "benchmark: h2o-get is standalone H2O only; Caster WEB is not used"

    (
        cd "$repo_dir"
        BENCH_HOST="$host" BENCH_PORT="$port" exec "$binary"
    ) >"$server_log" 2>&1 &

    server_pid=$!
    wait_for_health || {
        print_server_log_tail
        return 1
    }
    ensure_server_alive h2o-get
}

run_wrk() {
    label=$1
    stamp=$(date +%Y%m%d-%H%M%S)
    out="$results_dir/${label}_${stamp}.txt"

    mkdir -p "$results_dir"

    echo "benchmark: $label warmup $warmup at $base_url"
    if ! wrk -t"$threads" -c"$connections" -d"$warmup" -s "$wrk_script" "$base_url" >"$out.warmup.log" 2>&1; then
        echo "benchmark: $label warmup failed" >&2
        cat "$out.warmup.log" >&2
        print_server_log_tail
        return 1
    fi
    rm -f "$out.warmup.log"

    ensure_server_alive "$label"
    if ! health_check; then
        echo "benchmark: $label server stopped responding after warmup" >&2
        print_server_log_tail
        return 1
    fi

    echo "benchmark: $label duration=$duration connections=$connections threads=$threads"
    if ! wrk -t"$threads" -c"$connections" -d"$duration" -s "$wrk_script" "$base_url" >"$out" 2>&1; then
        cat "$out"
        echo "benchmark: $label measured run failed" >&2
        print_server_log_tail
        return 1
    fi

    cat "$out"
    ensure_server_alive "$label"
    echo "benchmark: wrote $out"
}

hey_url() {
    case "$hey_route" in
        health)
            echo "$base_url/health"
            ;;
        *)
            echo "benchmark: unsupported BENCH_HEY_ROUTE=$hey_route" >&2
            echo "benchmark: supported hey route: health" >&2
            return 2
            ;;
    esac
}

run_hey_once() {
    hey_once_label=$1
    hey_once_duration=$2
    hey_once_out=$3
    hey_once_url=$(hey_url)

    if ! hey -z "$hey_once_duration" -c "$connections" -cpus "$threads" -t "$hey_timeout" "$hey_once_url" >"$hey_once_out" 2>&1; then
        cat "$hey_once_out"
        echo "benchmark: $hey_once_label hey run failed" >&2
        print_server_log_tail
        return 1
    fi
}

run_hey() {
    hey_label=$1
    hey_stamp=$(date +%Y%m%d-%H%M%S)
    hey_out="$results_dir/${hey_label}_hey_${hey_route}_${hey_stamp}.txt"
    hey_warmup_out="$hey_out.warmup.log"
    hey_target_url=$(hey_url)

    mkdir -p "$results_dir"

    echo "benchmark: $hey_label hey warmup $warmup at $hey_target_url"
    run_hey_once "$hey_label" "$warmup" "$hey_warmup_out"
    rm -f "$hey_warmup_out"

    ensure_server_alive "$hey_label"
    if ! health_check; then
        echo "benchmark: $hey_label server stopped responding after warmup" >&2
        print_server_log_tail
        return 1
    fi

    echo "benchmark: $hey_label hey duration=$duration connections=$connections cpus=$threads route=$hey_route"
    run_hey_once "$hey_label" "$duration" "$hey_out"

    cat "$hey_out"
    ensure_server_alive "$hey_label"
    echo "benchmark: wrote $hey_out"
}

run_one() {
    label=$1

    cleanup

    case "$label" in
        caster)
            start_caster
            ;;
        fastapi)
            start_fastapi
            ;;
        go-http)
            start_go_http
            ;;
        go-http-router)
            start_go_http_router
            ;;
        h2o-get)
            start_h2o_get
            ;;
        *)
            usage
            exit 2
            ;;
    esac

    case "$bench_tool" in
        wrk)
            run_wrk "$label"
            ;;
        hey)
            run_hey "$label"
            ;;
        *)
            echo "benchmark: unsupported BENCH_TOOL=$bench_tool" >&2
            exit 2
            ;;
    esac
    cleanup
}

trap cleanup EXIT INT TERM

if [ -z "$target" ]; then
    usage
    exit 2
fi

need_cmd python3

case "$bench_tool" in
    wrk)
        if ! need_cmd wrk; then
            echo "install wrk first, for example: brew install wrk" >&2
            echo "on Debian/Ubuntu try: sudo apt-get install wrk" >&2
            exit 127
        fi
        ;;
    hey)
        if ! need_cmd hey; then
            echo "install hey first, for example: brew install hey" >&2
            echo "or: go install github.com/rakyll/hey@latest" >&2
            exit 127
        fi
        ;;
    *)
        echo "benchmark: unsupported BENCH_TOOL=$bench_tool" >&2
        exit 2
        ;;
esac

case "$target" in
    caster)
        run_one caster
        ;;
    fastapi)
        run_one fastapi
        ;;
    go-http)
        run_one go-http
        ;;
    go-http-router)
        run_one go-http-router
        ;;
    h2o-get)
        run_one h2o-get
        ;;
    both)
        run_one caster
        run_one fastapi
        ;;
    *)
        usage
        exit 2
        ;;
esac
