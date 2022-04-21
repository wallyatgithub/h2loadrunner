local server_id = start_server("../../maock/build/maock.json")

print("server id: ", server_id)

register_service_handler(server_id, "amf-reg", "handle_request", 20)

register_service_handler(server_id, "amf-dereg", "handle_request")

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
    local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8082", [":method"]="POST", [":path"]="/udm-ee/subscribe/12345"}
    local payload = "hello world again"

    send_http_request_and_await_response(request_headers_to_send, payload)
    
    
    local response_header = {[":status"]="204", ["user-agent"]="lua-server-script"}
    response_body = ""
    send_response(response_addr, response_header, response_body)
end

