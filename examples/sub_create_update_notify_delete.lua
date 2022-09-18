
start_server("maock_notify_server.json") -- this is the notify server simulating notify endpoint, you won't need this in your actual test, as it is the SUT

local server_id = start_server("maock_subscribe_update_delete.json") -- this is the nf producer handing subscribe, update, felete, and send notification

register_service_handler(server_id, "subscribe", "handle_subscribe", 50) -- change the request matching rules in maock_subscribe_update_delete.json based on your actual api

register_service_handler(server_id, "subs-update", "handle_subscribe_update", 50) -- ditto

register_service_handler(server_id, "subs-del", "handle_ubsubscribe", 50) -- ditto

local location_header_with_subs_id = {"/npcf-policyauthorization/v1/app-sessions/", "subs_id_placeholder"} -- change the first element based on your actual API definition

local timestamp_reporting_stat = 0 -- for statistics

local pseudo_uuid_as_worker_thread_id_for_stats_output = generate_uuid_v4() -- for statistics

local number_of_notify_request_sent = 0 -- for statistics

local number_of_notify_request_sent_last_second = 0 -- for statistics

math.randomseed(os.time()) -- initialize random seed for later use

-- utility functions begin

-- split path and query parameter
local function tokenize_path_and_query(path)
    path = path .. "?"
    results = string.gmatch(path, '([^?]+)')
    tokens = {}
    for res in results do
        table.insert(tokens, res)
    end
    return tokens
end

-- split path with / as seperator
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

-- handle subscribe
function handle_subscribe(response_addr, request_headers, request_payload)
    subscription_id = generate_uuid_v4() -- generate pseudo subscription id
    store_value(subscription_id, request_payload) -- store request body for update validation and for notification later, with subscription id as the key
    location_header_with_subs_id[2] = subscription_id -- fill subscription id into location header table
    response_header = {[":status"] = "201", ["location"] = table.concat(location_header_with_subs_id)} -- make the response headers
    send_response(response_addr, response_header, "") -- send back subscription response
end

function handle_subscribe_update(response_addr, request_headers, request_payload)
    path = request_headers[":path"] -- locate path header where we would locate the subscription id (we generated before)
    tokens = tokenize_path_and_query(path) -- some prep work here and next few lines to get subscription id
    path_without_query = tokens[1]
    path_tokens = tokenize_path(path_without_query)
    subscription_id = path_tokens[table.getn(path_tokens)] -- the last token is the subscription id
    subscribe_request_payload = get_value(subscription_id) -- retrieve the stored subscription request body saved before
    if (subscribe_request_payload == nil) -- no subscription found, returning 404
    then
        response_header = {[":status"] = "404"}
        send_response(response_addr, response_header, "")
        return
    end
    response_header = {[":status"] = "200"} -- else, subscription present (saved before), accept this update
    send_response(response_addr, response_header, '{"status": "update-success"}') -- send back 200

    -- send notify, here we send notify on every update, you can send fewer with the help of random number
    doc = rapidjson.Document()

    -- here we are decoding the request body of the original subscribe request to get the callback uri
    -- depending on your service logic, your callback uri can be updated in update request, then you need to decode the request_payload of this update
    ok, message = doc:parse(subscribe_request_payload)
    notify_uri = doc:get("/ascReqData/notifUri") -- locate the callback uri in the decode json with json pointer, change this based on your actual api definition
    notify_headers = parse_uri(notify_uri) -- parse_uri is a helper function provided by h2loadrunner to decode a uri to a table of headers

    notify_headers[":method"] = "POST"

    notify_response_headers, notify_response_payload = send_http_request_and_await_response(notify_headers, "") -- send the notification

    --send_http_request(notify_headers, "")

	-- next few lines are for notification request statistics.
    if (notify_response_headers ~= nil)
    then
        number_of_notify_request_sent = number_of_notify_request_sent + 1
    end
    now = time_since_epoch()
    if (now - timestamp_reporting_stat > 1000)
    then
	    -- For simplicity, every worker thread would print independent stats, maybe some enhancement can be done to aggregate
        output = string.format("thread: %s, number request sent: %d, tps = %d", pseudo_uuid_as_worker_thread_id_for_stats_output, number_of_notify_request_sent, ((number_of_notify_request_sent - number_of_notify_request_sent_last_second)*1000)/(now - timestamp_reporting_stat))
        number_of_notify_request_sent_last_second = number_of_notify_request_sent;
        timestamp_reporting_stat = now
        print (output)
    end
end

-- handle unsubscribe
function handle_ubsubscribe(response_addr, request_headers, request_payload)
    path = request_headers[":path"]
    tokens = tokenize_path_and_query(path)
    path_without_query = tokens[1]
    path_tokens = tokenize_path(path_without_query)
    subscription_id = path_tokens[table.getn(path_tokens)] -- locate subscription id
    subscribe_request_payload = delete_value(subscription_id) -- locate and delete the saved key-value (subscription id - request body)
    if (subscribe_request_payload == nil) -- no saved subscription before, or already deleted, return 404
    then
        response_header = {[":status"] = "404"}
        send_response(response_addr, response_header, "")
        return
    end
    response_header = {[":status"] = "204", ["x-who-am-i"] = "I am a piece of script"} -- return 204 for successful ubsubscribe
    send_response(response_addr, response_header, "")
end
