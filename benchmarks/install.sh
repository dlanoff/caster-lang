#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
venv_dir="$script_dir/.venv"

if ! command -v python3 >/dev/null 2>&1; then
    echo "benchmark install: python3 is required" >&2
    exit 127
fi

if [ ! -d "$venv_dir" ]; then
    python3 -m venv "$venv_dir"
fi

"$venv_dir/bin/python" -m pip install --upgrade pip
"$venv_dir/bin/python" -m pip install -r "$script_dir/requirements.txt"

if ! command -v wrk >/dev/null 2>&1; then
    echo "benchmark install: wrk is still required for the load test" >&2
    echo "macOS: brew install wrk" >&2
    echo "Debian/Ubuntu: sudo apt-get install wrk" >&2
    exit 127
fi

if ! command -v go >/dev/null 2>&1; then
    echo "benchmark install: Go is required for the go-http benchmark target" >&2
    echo "macOS: brew install go" >&2
    echo "Debian/Ubuntu: sudo apt-get install golang-go" >&2
    exit 127
fi

if ! command -v hey >/dev/null 2>&1; then
    echo "benchmark install: hey is required for the h2o-get benchmark target" >&2
    echo "macOS: brew install hey" >&2
    echo "or: go install github.com/rakyll/hey@latest" >&2
    exit 127
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "benchmark install: cmake is required for the h2o-get benchmark target" >&2
    echo "macOS: brew install cmake" >&2
    echo "Debian/Ubuntu: sudo apt-get install cmake" >&2
    exit 127
fi

if [ ! -d "$repo_dir/vendor/h2o" ]; then
    echo "benchmark install: missing H2O source at $repo_dir/vendor/h2o" >&2
    exit 1
fi

make -C "$repo_dir" -f build/Makefile h2o

"$repo_dir/caster" check --json "$script_dir/bankserver.cast"

mkdir -p "$script_dir/.go"
go build -o "$script_dir/.go/go-http-bankserver" "$script_dir/go_http_bankserver.go"

echo "benchmark install: ready"
