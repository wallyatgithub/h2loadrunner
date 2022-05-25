local server_id = start_server("maock_root.json")

print("server id: ", server_id)

register_service_handler(server_id, "POST", "handle_request", 20)

clusters = {"10.242.143.77:50051"}

math.randomseed(os.time())

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
        index = math.random(table.getn(clusters))
        headers[":authority"] = clusters[index]

    response_header, response_body, trailer = send_http_request_and_await_response(headers, payload)
    forward_response(response_addr, response_header, response_body, trailer)
end
