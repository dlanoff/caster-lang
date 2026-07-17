# Caster Bank Server Benchmark

This directory benchmarks `benchmarks/bankserver.cast` against FastAPI and Go `net/http` mirrors with the same routes and deterministic CPU work.

The benchmark uses `wrk`, a standard HTTP load-testing tool. The workload is mixed through `wrk_bank_mixed.lua` so each run exercises:

- `GET /health`
- `GET /accounts/{id}`
- `GET /accounts/{id}/balance`
- `GET /accounts/{id}/transactions`
- `POST /accounts/{id}/transfers`
- `POST /accounts/{id}/batch-transfers`
- `GET /search/{prefix}`
- `GET /compute/{id}`

## Install

From the repo root:

```sh
make run install
```

That creates `benchmarks/.venv`, installs the FastAPI dependencies, checks the benchmark-local Caster file, and verifies that `wrk` and Go exist.

If `wrk` is missing, install it:

```sh
brew install wrk
```

On Debian/Ubuntu, try:

```sh
sudo apt-get install wrk
```

If Go is missing, install it:

```sh
brew install go
```

On Debian/Ubuntu, try:

```sh
sudo apt-get install golang-go
```

## Run

Run FastAPI:

```sh
make run test fastapi
```

Run Caster:

```sh
make run caster
```

Run Go `net/http`:

```sh
make run go-http
```

Run Go `net/http` through a generic route table:

```sh
make run go-http-router
```

Run the standalone H2O `/health` ceiling:

```sh
make run h2o-get
```

Run Caster and FastAPI back to back:

```sh
make run test both
```

The lower-level script still accepts explicit benchmark settings:

```sh
benchmarks/run_benchmark.sh both 30s 128 4
benchmarks/run_benchmark.sh go-http 30s 128 4
benchmarks/run_benchmark.sh go-http-router 30s 128 4
benchmarks/run_benchmark.sh h2o-get 30s 128 4
```

The runner starts each server on `127.0.0.1:8000`, waits for `/health`, performs a warmup, runs `wrk`, writes output under `benchmarks/results/`, then stops the server.
Server stdout/stderr is also captured under `benchmarks/results/`. If a server exits during startup, warmup, or the measured run, the runner prints the last log lines before failing.

Environment overrides:

```sh
BENCH_HOST=127.0.0.1 BENCH_PORT=8000 BENCH_WARMUP=5s benchmarks/run_benchmark.sh both
```

## Notes

- Caster is launched with `./caster benchmarks/bankserver.cast`, so generated C and the executable go under the repo-root `.caster/` folder.
- FastAPI is launched as one `uvicorn` worker to keep the first comparison simple and predictable.
- Go is built from `benchmarks/go_http_bankserver.go` with the standard toolchain, and the binary goes under `benchmarks/.go/`.
- `go-http` is the direct `net/http` switch-router ceiling.
- `go-http-router` uses a small generic route table with precompiled path parts and params to better mirror Caster WEB routing.
- Both Go targets run with `GOMAXPROCS=1` so this comparison starts from a single-threaded Go runtime.
- `h2o-get` is a standalone H2O-only `/health` server used as a GET-only transport ceiling. It does not use Caster WEB.
- This is a local-loopback throughput and latency benchmark. It does not include TLS, external network effects, database I/O, or multi-process tuning.
