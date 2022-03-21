print("test.lua")

local conn_id = lua_connect_to_uri("http://192.168.1.124:8080")

print ("conn_id:", conn_id)

request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world"

send_http_request(request_headers_to_send, payload)

request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world again"

send_http_request(request_headers_to_send, payload)