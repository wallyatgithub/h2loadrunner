local server_id = start_server("maock_subscribe_update_delete.json")

register_service_handler(server_id, "subscribe", "handle_subscribe", 20)

register_service_handler(server_id, "subs-update", "handle_subscribe_update", 20)

register_service_handler(server_id, "subs-del", "handle_ubsubscribe", 20)

local location_header_with_subs_id = {"/some-hardcoded-api-root/some-service/v2/some-operation", "/", "subs_id_placeholder"}

local timestamp_reporting_stat = 0

local pseudo_uuid_as_worker_thread_id_for_stats_output = generate_uuid_v4()

local number_of_notify_request_sent = 0

local number_of_notify_request_sent_last_second = 0

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
-- utility functions end

function handle_subscribe(response_addr, request_headers, request_payload)
    subscription_id = generate_uuid_v4()
    store_value(subscription_id, request_payload)
    location_header_with_subs_id[2] = subscription_id
    response_header = {[":status"] = "201", ["location"] = table.concat(location_header_with_subs_id)}
    send_response(response_addr, response_header, "")
end

function handle_subscribe_update(response_addr, request_headers, request_payload)
    path = headers[":path"]
    tokens = tokenize_path_and_query(path)
    path_without_query = tokens[1]
    path_tokens = tokenize_path(path_without_query)
    subscription_id = path_tokens[table.getn(path_tokens)]
    subscribe_request_payload = get_value(subscription_id)
    if (subscribe_request_payload == nil)
    then
        response_header = {[":status"] = "404"}
        send_response(response_addr, response_header, "")
        return
    end
    response_header = {[":status"] = "200"}
    send_response(response_addr, response_header, '{"status": "success"}')

    -- send notify
    doc = rapidjson.Document()
    ok, message = doc:parse(subscribe_request_payload)
    notify_uri = doc:get("/callback-uri")
    
    notify_headers = parse_uri(notify_uri)
    
    notify_response_headers, notify_response_payload = send_http_request_and_await_response(notify_headers, "")
    
    if (notify_response_headers ~= nil)
    then
        number_of_notify_request_sent = number_of_notify_request_sent + 1
    end
    now = time_since_epoch()
    if (now - timestamp_reporting_stat > 1000)
    then
        output = string.format("thread: %s, number request sent: %d, tps = %d", pseudo_uuid_as_worker_thread_id_for_stats_output, number_of_notify_request_sent, ((number_of_request_sent - number_of_notify_request_sent_last_second)*1000)/(now - timestamp_reporting_stat))
        number_of_notify_request_sent_last_second = number_of_notify_request_sent;
        timestamp_reporting_stat = now
        print (output)
    end
end

function handle_ubsubscribe(response_addr, request_headers, request_payload)
    path = headers[":path"]
    tokens = tokenize_path_and_query(path)
    path_without_query = tokens[1]
    path_tokens = tokenize_path(path_without_query)
    subscription_id = path_tokens[table.getn(path_tokens)]
    subscribe_request_payload = get_value(subscription_id)
    if (subscribe_request_payload == nil)
    then
        response_header = {[":status"] = "404"}
        send_response(response_addr, response_header, "")
        return
    end
    response_header = {[":status"] = "204"}
    delete_value(subscription_id)
    send_response(response_addr, response_header, "")
end
