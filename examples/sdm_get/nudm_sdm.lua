local server_id = start_server("maock_nudm_sdm_get.json")

print("server id: ", server_id)

register_service_handler(server_id, "nudm_sdm_get", "handle_request", 20)

local nUDRs = {"127.0.0.1:8082"}

local udr_query = {"/some-hardcoded-api-root/nudr-dr/v2/subscription-data", "/", "{ueId}", "/", "{servingPlmnId}", "/provisioned-data?dataset-names=", "AM"}

local multiple_dataset_get_resp_amdata = {'{"amdata":', 'amdata-content', '}'}

local timestamp_reporting_stat = 0

local pseudo_uuid = generate_uuid_v4()

local number_of_request_sent = 0

local number_of_request_sent_last_second = 0

math.randomseed(os.time())

-- utility functions begin
local function tokenize_path_and_query(path)
    path = path .. "?"
    results = string.gmatch(path, '([^?]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

local function tokenize_path(path)
    path = path .. "/"
    results = string.gmatch(path, '([^/]+)')
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
    path = path .. "&"
    results = string.gmatch(path, '([^&]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

local function split_query(query)
    query = query .. "="
    results = string.gmatch(query, '([^=]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end
-- utility functions end

function handle_request(response_addr, headers, payload)
    now = time_since_epoch()
    if (now - timestamp_reporting_stat > 1000)
    then
        output = string.format("thread: %s, number request sent: %d, tps = %d", pseudo_uuid, number_of_request_sent, ((number_of_request_sent - number_of_request_sent_last_second)*1000)/(now - timestamp_reporting_stat))
        number_of_request_sent_last_second = number_of_request_sent;
        timestamp_reporting_stat = now
        print (output)
    end
    --print ("path:", headers[":path"])
    --index = math.random(table.getn(clusters))
    path = headers[":path"]
    tokens = tokenize_path_and_query(path)
    path_without_query = tokens[1]
    sdm_query = tokens[2]
    path_tokens = tokenize_path(path_without_query)
    supi = path_tokens[table.getn(path_tokens)]
    data_set = ""
    serving_plmn_id = ""
    query_tokens = tokenize_query(sdm_query)

    for k, v in pairs(query_tokens) do
        if (startswith(v, "dataset-names"))
        then
            data_set = split_query(v)[2]
        elseif (startswith(v, "plmn-id"))
        then
            serving_plmn_id = split_query(v)[2]
        end
    end

    udr_query[3] = supi
    udr_query[5] = serving_plmn_id
    udr_query[7] = data_set
    
    headers[":path"] = table.concat(udr_query)
    index = math.random(table.getn(nUDRs))
    headers[":authority"] = nUDRs[index]
    
    headers[":method"] = "GET"

    udr_response_header, response_body = send_http_request_and_await_response(headers, "")
    
    local doc = rapidjson.Document()
    local ok, message = doc:parse(response_body)
    local activeTime = doc:get("/activeTime")
    number_of_request_sent = number_of_request_sent + 1

    multiple_dataset_get_resp_amdata[2] = response_body;

    local udm_resp_header = {[":status"] = "200", ["x-who-am-I"] = "I am a powerful UDM-SDM instance running Lua script", ["activeTime-from-json"] = tostring(activeTime)}
    
    send_response(response_addr, udm_resp_header, table.concat(multiple_dataset_get_resp_amdata))
end
