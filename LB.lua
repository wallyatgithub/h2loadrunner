local server_id = start_server("../maock_root.json")

print("server id: ", server_id)

register_service_handler(server_id, "POST", "handle_request", 20)

dest_host = "localhost"

math.randomseed(os.time())

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
    clusters = resolve_hostname(dest_host)
    if ((clusters == nil) or (table.getn(clusters) == 0))
    then
        response_header = {[":status"] = "503"}
        response_body = "gateway not found"
        send_response(response_addr, response_header, response_body)
        return
    end

    index = math.random(table.getn(clusters))
    headers["x-envoy-original-dst-host"] = clusters[index]
    headers["x-envoy-original-dst-host"] = headers["x-envoy-original-dst-host"]..":8082"

    response_header, response_body = send_http_request_and_await_response(headers, payload, 1000) -- 1000 is the response timeout

    local next = next
    if (next(response_header) == nil)
    then
        response_header = {[":status"] = "503"}
        response_body = "gateway not reachable"
    end

    send_response(response_addr, response_header, response_body)
end

