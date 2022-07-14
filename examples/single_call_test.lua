
local request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.107:8081", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
local payload = "hello world again"

local response_header, response_payload = send_http_request_and_await_response(request_headers_to_send, payload)

print (response_payload)