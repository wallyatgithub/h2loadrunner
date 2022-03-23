print("test.lua")

setup_parallel_test(2, 4)

local client_id = make_connection("http://192.168.1.124:8080")

print ("client_id:", client_id)

local client_id = make_connection("http://192.168.1.124:8080")

print ("client_id:", client_id)

--[[
local client_id = make_connection("http://192.168.1.124:8088")

print ("client_id:", client_id)

local client_id = make_connection("http://192.168.1.124:8088")

print ("client_id:", client_id)
--]]

request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world"

local client_id, stream_id = send_http_request(request_headers_to_send, payload)

print ("client_id:", client_id)
print ("stream_id:", stream_id)

local headers, body = await_response(client_id, stream_id)
print ("status code:", headers[":status"])
print ("body:", body)

request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world again"

local client_id, stream_id = send_http_request(request_headers_to_send, payload)

print ("client_id:", client_id)
print ("stream_id:", stream_id)

local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)

print ("status code:", headers[":status"])
print ("body:", body)

sleep_for_ms(2000)