package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strconv"
	"strings"
)

func main() {
	host := getenv("BENCH_HOST", "127.0.0.1")
	port := getenv("BENCH_PORT", "8000")
	addr := host + ":" + port
	handler := http.HandlerFunc(route)

	if getenv("BENCH_GO_ROUTER", "direct") == "table" {
		handler = http.HandlerFunc(routeTable)
	}

	server := &http.Server{
		Addr:    addr,
		Handler: handler,
	}

	log.Printf("go net/http benchmark server listening on %s", addr)
	log.Fatal(server.ListenAndServe())
}

func getenv(name string, fallback string) string {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	return value
}

func route(w http.ResponseWriter, r *http.Request) {
	path := strings.Trim(r.URL.Path, "/")
	parts := []string{}
	if path != "" {
		parts = strings.Split(path, "/")
	}

	switch {
	case len(parts) == 1 && parts[0] == "health":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		health(w)

	case len(parts) == 2 && parts[0] == "accounts":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		getAccount(w, parts[1])

	case len(parts) == 3 && parts[0] == "accounts" && parts[2] == "balance":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		getBalance(w, parts[1])

	case len(parts) == 3 && parts[0] == "accounts" && parts[2] == "transactions":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		getTransactions(w, parts[1])

	case len(parts) == 3 && parts[0] == "accounts" && parts[2] == "transfers":
		if !requireMethod(w, r, http.MethodPost) {
			return
		}
		postTransfer(w, r, parts[1])

	case len(parts) == 3 && parts[0] == "accounts" && parts[2] == "batch-transfers":
		if !requireMethod(w, r, http.MethodPost) {
			return
		}
		postBatchTransfers(w, r, parts[1])

	case len(parts) == 2 && parts[0] == "search":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		searchAccounts(w, parts[1])

	case len(parts) == 2 && parts[0] == "compute":
		if !requireMethod(w, r, http.MethodGet) {
			return
		}
		computeHeavy(w, parts[1])

	default:
		writeJSON(w, http.StatusNotFound, map[string]interface{}{
			"error": "not found",
		})
	}
}

type benchRouteHandler func(http.ResponseWriter, *http.Request, map[string]string)

type benchRoute struct {
	method  string
	parts   []string
	handler benchRouteHandler
}

var benchRoutes = []benchRoute{
	newBenchRoute(http.MethodGet, "/health", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		health(w)
	}),
	newBenchRoute(http.MethodGet, "/accounts/:id", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		getAccount(w, params["id"])
	}),
	newBenchRoute(http.MethodGet, "/accounts/:id/balance", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		getBalance(w, params["id"])
	}),
	newBenchRoute(http.MethodGet, "/accounts/:id/transactions", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		getTransactions(w, params["id"])
	}),
	newBenchRoute(http.MethodPost, "/accounts/:id/transfers", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		postTransfer(w, r, params["id"])
	}),
	newBenchRoute(http.MethodPost, "/accounts/:id/batch-transfers", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		postBatchTransfers(w, r, params["id"])
	}),
	newBenchRoute(http.MethodGet, "/search/:prefix", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		searchAccounts(w, params["prefix"])
	}),
	newBenchRoute(http.MethodGet, "/compute/:id", func(w http.ResponseWriter, r *http.Request, params map[string]string) {
		computeHeavy(w, params["id"])
	}),
}

func newBenchRoute(method string, pattern string, handler benchRouteHandler) benchRoute {
	return benchRoute{
		method:  method,
		parts:   pathParts(pattern),
		handler: handler,
	}
}

func routeTable(w http.ResponseWriter, r *http.Request) {
	parts := pathParts(r.URL.Path)
	foundPath := false

	for _, route := range benchRoutes {
		params, ok := matchParts(route.parts, parts)
		if !ok {
			continue
		}

		if route.method == r.Method {
			route.handler(w, r, params)
			return
		}

		foundPath = true
	}

	if foundPath {
		writeJSON(w, http.StatusMethodNotAllowed, map[string]interface{}{
			"error": "method not allowed",
		})
		return
	}

	writeJSON(w, http.StatusNotFound, map[string]interface{}{
		"error": "not found",
	})
}

func pathParts(path string) []string {
	path = strings.Trim(path, "/")
	if path == "" {
		return nil
	}
	return strings.Split(path, "/")
}

func matchParts(pattern []string, actual []string) (map[string]string, bool) {
	if len(pattern) != len(actual) {
		return nil, false
	}

	var params map[string]string
	for i, expected := range pattern {
		if strings.HasPrefix(expected, ":") {
			if params == nil {
				params = map[string]string{}
			}
			params[expected[1:]] = actual[i]
			continue
		}

		if expected != actual[i] {
			return nil, false
		}
	}

	return params, true
}

func requireMethod(w http.ResponseWriter, r *http.Request, method string) bool {
	if r.Method == method {
		return true
	}

	w.Header().Set("Allow", method)
	writeJSON(w, http.StatusMethodNotAllowed, map[string]interface{}{
		"error": "method not allowed",
	})
	return false
}

func writeJSON(w http.ResponseWriter, status int, value interface{}) {
	body, err := json.Marshal(value)
	if err != nil {
		http.Error(w, "json encode failed", http.StatusInternalServerError)
		return
	}

	headers := w.Header()
	headers.Set("Content-Type", "application/json")
	headers.Set("Content-Length", strconv.Itoa(len(body)))
	headers["Date"] = nil
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func readJSONBody(r *http.Request) map[string]interface{} {
	defer r.Body.Close()

	var body map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil || body == nil {
		return map[string]interface{}{}
	}

	return body
}

func intValue(body map[string]interface{}, key string, fallback int) int {
	value, ok := body[key]
	if !ok || value == nil {
		return fallback
	}

	switch typed := value.(type) {
	case int:
		return typed
	case int64:
		return int(typed)
	case float64:
		return int(typed)
	case json.Number:
		n, err := typed.Int64()
		if err == nil {
			return int(n)
		}
	case string:
		n, err := strconv.Atoi(typed)
		if err == nil {
			return n
		}
	}

	return fallback
}

func strValue(body map[string]interface{}, key string, fallback string) string {
	value, ok := body[key]
	if !ok || value == nil {
		return fallback
	}

	switch typed := value.(type) {
	case string:
		return typed
	default:
		return fmt.Sprint(typed)
	}
}

func accountSeed(accountID string) int {
	seed := 17
	accountLen := len(accountID)

	for i := range accountID {
		seed = ((seed * 131) + i + accountLen) % 1000003
	}

	return seed
}

func fakeBalance(accountID string) int {
	seed := accountSeed(accountID)
	balance := 100000 + seed

	for i := 0; i < 250; i++ {
		balance = ((balance * 17) + seed + 91) % 10000000
	}

	return balance
}

func fakeRiskScore(accountID string, amount int) int {
	seed := accountSeed(accountID)
	score := seed + amount

	for i := 0; i < 600; i++ {
		score = ((score * 31) + amount + 7) % 100000
	}

	return score % 100
}

func fakeLedgerChecksum(fromID string, toID string, amount int) int {
	checksum := accountSeed(fromID) + accountSeed(toID) + amount

	for i := 0; i < 1200; i++ {
		checksum = ((checksum * 53) + amount + len(fromID) + len(toID)) % 1000000007
	}

	return checksum
}

func health(w http.ResponseWriter) {
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"ok":      true,
		"service": "caster-bank",
		"status":  "healthy",
	})
}

func getAccount(w http.ResponseWriter, accountID string) {
	balance := fakeBalance(accountID)
	risk := fakeRiskScore(accountID, 0)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"id":       accountID,
		"kind":     "checking",
		"currency": "USD",
		"balance":  balance,
		"risk":     risk,
		"active":   true,
	})
}

func getBalance(w http.ResponseWriter, accountID string) {
	balance := fakeBalance(accountID)

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"id":        accountID,
		"balance":   balance,
		"available": balance - 2500,
		"currency":  "USD",
	})
}

func getTransactions(w http.ResponseWriter, accountID string) {
	seed := accountSeed(accountID)

	credits := 0
	debits := 0
	largest := 0
	checksum := seed

	for i := 0; i < 1000; i++ {
		amount := ((seed + i*37) % 50000) + 1

		if i%2 == 0 {
			credits += amount
		} else {
			debits += amount
		}

		if amount > largest {
			largest = amount
		}

		checksum = ((checksum * 29) + amount + i) % 1000000007
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"id":       accountID,
		"count":    1000,
		"credits":  credits,
		"debits":   debits,
		"largest":  largest,
		"checksum": checksum,
	})
}

func postTransfer(w http.ResponseWriter, r *http.Request, accountID string) {
	body := readJSONBody(r)

	toID := strValue(body, "to", "unknown")
	amount := intValue(body, "amount", 0)

	if amount <= 0 {
		writeJSON(w, http.StatusBadRequest, map[string]interface{}{
			"error": "amount must be positive",
		})
		return
	}

	balance := fakeBalance(accountID)

	if amount > balance {
		writeJSON(w, http.StatusConflict, map[string]interface{}{
			"error":   "insufficient funds",
			"balance": balance,
			"amount":  amount,
		})
		return
	}

	risk := fakeRiskScore(accountID, amount)
	checksum := fakeLedgerChecksum(accountID, toID, amount)

	if risk > 92 {
		writeJSON(w, http.StatusForbidden, map[string]interface{}{
			"error":    "transfer blocked by risk engine",
			"risk":     risk,
			"checksum": checksum,
		})
		return
	}

	writeJSON(w, http.StatusCreated, map[string]interface{}{
		"ok":        true,
		"from":      accountID,
		"to":        toID,
		"amount":    amount,
		"remaining": balance - amount,
		"risk":      risk,
		"checksum":  checksum,
	})
}

func postBatchTransfers(w http.ResponseWriter, r *http.Request, accountID string) {
	body := readJSONBody(r)

	count := intValue(body, "count", 100)
	amount := intValue(body, "amount", 25)
	toID := strValue(body, "to", "batch-destination")

	if count <= 0 {
		writeJSON(w, http.StatusBadRequest, map[string]interface{}{
			"error": "count must be positive",
		})
		return
	}

	accepted := 0
	rejected := 0
	checksum := 0

	for i := 0; i < count; i++ {
		nextAmount := amount + (i % 17)
		risk := fakeRiskScore(accountID, nextAmount)

		if risk > 94 {
			rejected++
		} else {
			accepted++
		}

		checksum += fakeLedgerChecksum(accountID, toID, nextAmount)
		checksum %= 1000000007
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"ok":        true,
		"from":      accountID,
		"to":        toID,
		"requested": count,
		"accepted":  accepted,
		"rejected":  rejected,
		"checksum":  checksum,
	})
}

func searchAccounts(w http.ResponseWriter, prefix string) {
	total := 0
	checksum := accountSeed(prefix)

	for i := 0; i < 2000; i++ {
		rowScore := ((checksum * 41) + i + len(prefix)) % 1000003

		if rowScore%3 == 0 {
			total++
		}

		checksum = rowScore
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"prefix":   prefix,
		"scanned":  2000,
		"matches":  total,
		"checksum": checksum,
	})
}

func computeHeavy(w http.ResponseWriter, accountID string) {
	seed := accountSeed(accountID)
	value := seed

	for i := 0; i < 10000; i++ {
		value = ((value * 67) + i + seed) % 1000000007
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{
		"id":         accountID,
		"iterations": 10000,
		"result":     value,
	})
}
