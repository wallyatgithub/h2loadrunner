local server_id = start_server("../../maock/build/maock.json")

print("server id: ", server_id)

register_service_handler(server_id, "subscribe", "handle_request")

function handle_request(response_addr, headers, payload)
    print ("path:", headers[":path"])
    local response_header = {[":status"]="201", ["user-agent"]="lua-server-script", ["location"]="/udm-ee/subscription/fdfad"}
    local response_body = "hello, this is from lua script"
    send_response(response_addr, response_header, response_body)
end

