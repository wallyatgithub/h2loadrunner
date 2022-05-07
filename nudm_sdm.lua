local server_id = start_server("maock_nudm_sdm_get.json")

print("server id: ", server_id)

register_service_handler(server_id, "nudm-sdm-get", "handle_request", 20)

nUDRs = {"10.67.34.200:8080", "10.67.34.200:8081", "10.67.34.200:8082"}

math.randomseed(os.time())

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
        index = math.random(table.getn(clusters))
        headers[":authority"] = clusters[index]

    response_header, response_body = send_http_request_and_await_response(headers, payload)

    send_response(response_addr, response_header, response_body)
end

