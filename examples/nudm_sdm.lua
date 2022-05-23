local server_id = start_server("maock_nudm_sdm_get.json")

print("server id: ", server_id)

register_service_handler(server_id, "nudm-sdm-get", "handle_request", 20)

nUDRs = {"192.168.1.107:8082"}

grpc_server = {"192.168.1.107:8081"}

math.randomseed(os.time())

local function tokenize_path(path)
    local path = path .. "?"
    local results = string.gmatch(path, '([^?]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

local function startswith(text, prefix)
    return text:find(prefix, 1, true) == 1
end

local function tokenize_query(path)
    local path = path .. "?"
    local results = string.gmatch(path, '([^&]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

local function split_query(query)
    local query = query .. "="
    local results = string.gmatch(query, '([^=]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
    --index = math.random(table.getn(clusters))
    local path = headers[":path"]
    local tokens = tokenize_path(path)
    local supi = string.sub(tokens[1], 2)
    local data_set

    local query_tokens = tokenize_query(tokens[2])

    for k, v in pairs(query_tokens) do
        if (startswith(v, "dataset-names"))
        then
            data_set = split_query(v)[2]
        end
    end

-- to be continued

    headers[":authority"] = nUDRs[1]

    response_header, response_body = send_http_request_and_await_response(headers, payload)

    send_response(response_addr, response_header, response_body)
end

