print("Test starts")

--[[
This is to set up the number of threads and coroutines
First argument: number of OS threads
Second argument: total number of Lua coroutines accross all threads
Within one thread, to the same host, only one connection exists
Obviously, there are different connections to different hosts even within each thread
Coroutine helps to increase concurrency
You can send out 2 concurrent requests and await for responses from two different coroutines
Every coroutine executes the same script (this script)
This function returns an unique id of this coroutine, and you can use it to customize the behavior of this coroutine
For example, you can use this id to decide which user(s) this coroutine should use to construct the request
--]]
local my_id = setup_parallel_test(10, 2000)

--[[
Information purpose, not necessary
--]]
print ("my_id:", my_id)

--[[
optional, this is to show how to make a connection beforehand
--]]
local client_id = make_connection("http://192.168.1.124:8080")
--[[
Information purpose, not necessary
--]]
print ("client_id:", client_id)

--[[
-- A connection established before will be reused and returned
local client_id = make_connection("http://192.168.1.124:8080")
print ("client_id:", client_id)
--]]

--[[
-- for debug, to show a failure connection, failure connection will be retried until test finish
local client_id = make_connection("http://192.168.1.124:8088")
print ("client_id:", client_id)

-- for debug, to show the re-use of a failure connection
local client_id = make_connection("http://192.168.1.124:8088")
print ("client_id:", client_id)
--]]

--[[
this is to show how to prepare a request message, a table of headers and a payload
--]]
request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="POST", [":path"]="/nudm-uecm/test"}
payload = "hello world"

--[[
This is to show how to send a request
This function will return immediately and will not pause the coroutine
if no connection exists, a new connection will be made to send this request
--]]
local client_id, stream_id = send_http_request(request_headers_to_send, payload)

--[[
Information purpose, not necessary
--]]
--print ("client_id:", client_id)
--print ("stream_id:", stream_id)

--[[
This is to show how to sleep for some milli seconds, this will only pause this Lua coroutine, not the OS thread
--]]
--sleep_for_ms(1000)

--[[
This is to show how to retrieve the response of a request sent before
Pass client_id and stream_id to this function to retrieve the response
If the response is not yet available, it will pause this coroutine and wait until the response is available.
So obviously, there is no need to call sleep_for_ms before to wait for the response to be available
--]]
local headers, body = await_response(client_id, stream_id)
--[[
Information purpose, not necessary
--]]
--print ("status code:", headers[":status"])
--print ("body:", body)

--sleep_for_ms(1000)

--[[
This is to show how to send requests repeatedly while with a pause in between
--]]
for i=1,100 do
    request_headers_to_send = {[":scheme"]="http", [":authority"]="192.168.1.124:8080", [":method"]="PATCH", [":path"]="/nudm-uecm/test"}
    payload = "hello world again"
    local client_id, stream_id = send_http_request(request_headers_to_send, payload)
    sleep_for_ms(100)
    --print ("client_id:", client_id)
    --print ("stream_id:", stream_id)
end
--sleep_for_ms(1000)

--[[
This is to show how send a request and wait for the response with one function
This function will send out the request, and pause this coroutine
One the response is received, this coroutine is resumed and the response is returned
--]]
local headers, body = send_http_request_and_await_response(request_headers_to_send, payload)

print ("status code:", headers[":status"])
print ("body:", body)

--sleep_for_ms(2000)