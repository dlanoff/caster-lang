from typing import Any

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse


app = FastAPI(
    title="Caster Bank Benchmark Mirror",
    docs_url=None,
    redoc_url=None,
)


def account_seed(account_id: str) -> int:
    seed = 17
    account_len = len(account_id)

    for i, _ in enumerate(account_id):
        seed = ((seed * 131) + i + account_len) % 1_000_003

    return seed


def fake_balance(account_id: str) -> int:
    seed = account_seed(account_id)
    balance = 100_000 + seed

    for _ in range(250):
        balance = ((balance * 17) + seed + 91) % 10_000_000

    return balance


def fake_risk_score(account_id: str, amount: int) -> int:
    seed = account_seed(account_id)
    score = seed + amount

    for _ in range(600):
        score = ((score * 31) + amount + 7) % 100_000

    return score % 100


def fake_ledger_checksum(from_id: str, to_id: str, amount: int) -> int:
    checksum = account_seed(from_id) + account_seed(to_id) + amount

    for _ in range(1_200):
        checksum = ((checksum * 53) + amount + len(from_id) + len(to_id)) % 1_000_000_007

    return checksum


def coerce_int(value: Any, fallback: int) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def coerce_str(value: Any, fallback: str) -> str:
    if value is None:
        return fallback
    return str(value)


async def json_body(request: Request):
    try:
        body = await request.json()
    except Exception:
        return {}

    if isinstance(body, dict):
        return body

    return {}


@app.get("/health")
def health():
    return {
        "ok": True,
        "service": "caster-bank",
        "status": "healthy",
    }


@app.get("/accounts/{account_id}")
def get_account(account_id: str):
    balance = fake_balance(account_id)
    risk = fake_risk_score(account_id, 0)

    return {
        "id": account_id,
        "kind": "checking",
        "currency": "USD",
        "balance": balance,
        "risk": risk,
        "active": True,
    }


@app.get("/accounts/{account_id}/balance")
def get_balance(account_id: str):
    balance = fake_balance(account_id)

    return {
        "id": account_id,
        "balance": balance,
        "available": balance - 2_500,
        "currency": "USD",
    }


@app.get("/accounts/{account_id}/transactions")
def get_transactions(account_id: str):
    seed = account_seed(account_id)

    credits = 0
    debits = 0
    largest = 0
    checksum = seed

    for i in range(1_000):
        amount = ((seed + i * 37) % 50_000) + 1

        if i % 2 == 0:
            credits += amount
        else:
            debits += amount

        if amount > largest:
            largest = amount

        checksum = ((checksum * 29) + amount + i) % 1_000_000_007

    return {
        "id": account_id,
        "count": 1_000,
        "credits": credits,
        "debits": debits,
        "largest": largest,
        "checksum": checksum,
    }


@app.post("/accounts/{account_id}/transfers")
async def post_transfer(account_id: str, request: Request):
    body = await json_body(request)

    to_id = coerce_str(body.get("to"), "unknown")
    amount = coerce_int(body.get("amount"), 0)

    if amount <= 0:
        return JSONResponse({"error": "amount must be positive"}, status_code=400)

    balance = fake_balance(account_id)

    if amount > balance:
        return JSONResponse(
            {
                "error": "insufficient funds",
                "balance": balance,
                "amount": amount,
            },
            status_code=409,
        )

    risk = fake_risk_score(account_id, amount)
    checksum = fake_ledger_checksum(account_id, to_id, amount)

    if risk > 92:
        return JSONResponse(
            {
                "error": "transfer blocked by risk engine",
                "risk": risk,
                "checksum": checksum,
            },
            status_code=403,
        )

    return JSONResponse(
        {
            "ok": True,
            "from": account_id,
            "to": to_id,
            "amount": amount,
            "remaining": balance - amount,
            "risk": risk,
            "checksum": checksum,
        },
        status_code=201,
    )


@app.post("/accounts/{account_id}/batch-transfers")
async def post_batch_transfers(account_id: str, request: Request):
    body = await json_body(request)

    count = coerce_int(body.get("count"), 100)
    amount = coerce_int(body.get("amount"), 25)
    to_id = coerce_str(body.get("to"), "batch-destination")

    if count <= 0:
        return JSONResponse({"error": "count must be positive"}, status_code=400)

    accepted = 0
    rejected = 0
    checksum = 0

    for i in range(count):
        next_amount = amount + (i % 17)
        risk = fake_risk_score(account_id, next_amount)

        if risk > 94:
            rejected += 1
        else:
            accepted += 1

        checksum += fake_ledger_checksum(account_id, to_id, next_amount)
        checksum = checksum % 1_000_000_007

    return {
        "ok": True,
        "from": account_id,
        "to": to_id,
        "requested": count,
        "accepted": accepted,
        "rejected": rejected,
        "checksum": checksum,
    }


@app.get("/search/{prefix}")
def search_accounts(prefix: str):
    total = 0
    checksum = account_seed(prefix)

    for i in range(2_000):
        row_score = ((checksum * 41) + i + len(prefix)) % 1_000_003

        if row_score % 3 == 0:
            total += 1

        checksum = row_score

    return {
        "prefix": prefix,
        "scanned": 2_000,
        "matches": total,
        "checksum": checksum,
    }


@app.get("/compute/{account_id}")
def compute_heavy(account_id: str):
    seed = account_seed(account_id)
    value = seed

    for i in range(10_000):
        value = ((value * 67) + i + seed) % 1_000_000_007

    return {
        "id": account_id,
        "iterations": 10_000,
        "result": value,
    }
