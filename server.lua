local server_id = start_server("../../maock/build/maock.json")

print("server id: ", server_id)

register_service_handler(server_id, "amf-reg", "handle_request", 4)

register_service_handler(server_id, "amf-dereg", "handle_request")

function handle_request(response_addr, headers, payload)
    --print ("path:", headers[":path"])
    local response_header = {[":status"]="204", ["user-agent"]="lua-server-script"}
    response_body = ""
    send_response(response_addr, response_header, response_body)
end

