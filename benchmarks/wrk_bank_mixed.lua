local counter = 0
local ids = {
  "acct-1001",
  "acct-1002",
  "acct-1003",
  "acct-1004",
  "acct-1005",
  "acct-1006",
  "acct-1007",
  "acct-1008"
}

wrk.headers["content-type"] = "application/json"

request = function()
  counter = counter + 1

  local id = ids[(counter % #ids) + 1]
  local route = counter % 8

  if route == 0 then
    return wrk.format("GET", "/health")
  end

  if route == 1 then
    return wrk.format("GET", "/accounts/" .. id)
  end

  if route == 2 then
    return wrk.format("GET", "/accounts/" .. id .. "/balance")
  end

  if route == 3 then
    return wrk.format("GET", "/accounts/" .. id .. "/transactions")
  end

  if route == 4 then
    local body = string.format('{"to":"dest-%d","amount":%d}', counter % 97, 100 + (counter % 900))
    return wrk.format("POST", "/accounts/" .. id .. "/transfers", nil, body)
  end

  if route == 5 then
    local body = string.format('{"to":"batch-%d","amount":25,"count":100}', counter % 31)
    return wrk.format("POST", "/accounts/" .. id .. "/batch-transfers", nil, body)
  end

  if route == 6 then
    return wrk.format("GET", "/search/acct")
  end

  return wrk.format("GET", "/compute/" .. id)
end
